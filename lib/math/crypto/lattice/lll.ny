;; Keywords: lattice lll
;; LLL reduction.
;; Reference:
;; - https://web.cs.elte.hu/~lovasz/scans/lll.pdf
;; - https://www.cs.cmu.edu/~afs/cs/project/quake/public/papers/Coppersmith-Crypto96.pdf
module std.math.crypto.lattice.lll(lll, gram_schmidt, lll_backend_report, gso_profile, gso_report, lll_quality_report, lll_is_reduced, lll_reduce_report, lll_report, lll_reduce_bounded, lll_reduce_bounded_report, lll_find_ternary_pair_rows, lll_find_ntru_key_rows, lll_basis_gram_gso_parity_report, lll_gso_parity_report, lll_gram_gso_report, lll_gram_quality_report, lll_gram_is_reduced, lll_gram_reduce_report, lll_gram_reduce_bounded_report, lll_gram_first_column_reduce_report, lll_gram_report, lll_gram)
use std.core
use std.core.str as str
use std.core.tbuf
use std.math.big (bigint_to_str, bigint_from_int, bigint, is_bigint, bigint_bit_length, bigint_to_int)
use std.math.integer (Z, gcd)
use std.math.matrix
use std.math.scalar (pow)
use std.os.clock (ticks)
use std.os.prim (env)

fn _lll_set_fields(dict: out, list: fields): dict {
   mut i = 0
   while(i < fields.len){
      def field = fields.get(i)
      out[to_str(field.get(0))] = field.get(1, nil)
      i += 1
   }
   out
}

fn _lll_rows(any: m): int { int(m[0]) }

fn _lll_cols(any: m): int { int(m[1]) }

fn _lll_data(any: m): list { m[2] }

fn _lll_big_float(any: x): f64 {
   __bigint_to_f64(Z(x))
}

fn _lll_float(any: x): f64 { is_bigint(x) ? _lll_big_float(x) : float(x) }

fn _lll_big_float_scaled(any: x, int: scale_digits): f64 {
   mut out = __bigint_to_f64(Z(x))
   if(scale_digits != 0){ out *= pow(10.0, 0.0 - float(scale_digits)) }
   out
}

fn _lll_float_scaled(any: x, int: scale_digits): f64 {
   if(is_bigint(x)){ return _lll_big_float_scaled(x, scale_digits) }
   float(x) * pow(10.0, 0.0 - float(scale_digits))
}

fn _lll_float_row_scaled(list: row, int: scale_digits): list {
   mut out = []
   mut i = 0
   while(i < row.len){
      out = out.append(_lll_float_scaled(row[i], scale_digits))
      i += 1
   }
   out
}

fn _lll_entry_digits(any: x): int {
   if(is_float(x)){ return 1 }
   if(is_int(x)){
      mut v = int(x)
      if(v < 0){
         if(v <= -1000000000000000000){ return 19 }
         v = 0 - v
      }
      if(v < 10){ return 1 }
      if(v < 100){ return 2 }
      if(v < 1000){ return 3 }
      if(v < 10000){ return 4 }
      if(v < 100000){ return 5 }
      if(v < 1000000){ return 6 }
      if(v < 10000000){ return 7 }
      if(v < 100000000){ return 8 }
      if(v < 1000000000){ return 9 }
      if(v < 10000000000){ return 10 }
      if(v < 100000000000){ return 11 }
      if(v < 1000000000000){ return 12 }
      if(v < 10000000000000){ return 13 }
      if(v < 100000000000000){ return 14 }
      if(v < 1000000000000000){ return 15 }
      if(v < 10000000000000000){ return 16 }
      if(v < 100000000000000000){ return 17 }
      if(v < 1000000000000000000){ return 18 }
      return 19
   }
   def z = Z(x)
   def a = z < Z(0) ? Z(0) - z : z
   bigint_to_str(a).len
}

fn _lll_scale_digits(any: basis): int {
   def data = _lll_data(basis)
   mut max_digits = 0
   mut i = 0
   while(i < data.len){
      def row = data[i]
      mut j = 0
      while(j < row.len){
         def d = _lll_entry_digits(row[j])
         if(d > max_digits){ max_digits = d }
         j += 1
      }
      i += 1
   }
   max(0, max_digits - 80)
}

fn _lll_get(any: m, int: i, int: j): any {
   def data = _lll_data(m)
   if(i < 0 || i >= data.len){ return Z(0) }
   def row = data[i]
   if(!is_list(row) || j < 0 || j >= row.len){ return Z(0) }
   row[j]
}

fn _lll_set(any: m, int: i, int: j, any: val): any {
   def rows = _lll_rows(m)
   def cols = _lll_cols(m)
   mut data = _lll_data(m)
   if(i < 0 || i >= rows || j < 0 || j >= cols){ return m }
   mut row = data[i]
   row[j] = val
   data[i] = row
   [rows, cols, data]
}

fn lll_backend_report(any: basis=nil): dict {
   "Return the LLL strategy policy and audit fields."
   if(basis == nil){ return _lll_backend_stub().set("threshold_hint", _lll_auto_threshold()) }
   {
      "default_method": "ny",
      "auto_method": "ny",
      "ny_default": true,
      "threshold_hint": _lll_auto_threshold(),
      "rows": _lll_rows(basis),
      "cols": _lll_cols(basis),
      "min_dim": _lll_min_dim(basis),
      "auto_order": "ny"
   }
}

fn gram_schmidt(any: basis): list {
   "Perform Gram-Schmidt orthogonalization on a matrix basis.
   Returns [b_star, mu]."
   def n = _lll_rows(basis)
   mut b_star = list(0)
   mut mu = matrix_zero(n, n)
   def basis_data = _lll_data(basis)
   def scale_digits = _lll_scale_digits(basis)
   mut i = 0
   while(i < n){
      mut v, j = _lll_float_row_scaled(basis_data[i], scale_digits), 0
      while(j < i){
         def bj = b_star[j]
         def den = dot_product(bj, bj)
         def den_zero = den == Z(0) || den == 0 || _lll_float(den) == 0.0
         def mu_ij = den_zero ? 0.0 : _lll_float(dot_product(v, bj)) / _lll_float(den)
         mu = _lll_set(mu, i, j, mu_ij)
         if(!den_zero){ v = vector_sub(v, vector_scale(bj, mu_ij)) }
         j += 1
      }
      b_star = b_star.append(v)
      i += 1
   }
   [b_star, mu]
}

fn _lll_min_dim(any: basis): int {
   def rows = _lll_rows(basis)
   def cols = _lll_cols(basis)
   rows < cols ? rows : cols
}

fn _lll_elapsed_ms(any: t0): f64 { float(ticks() - t0) / 1000000.0 }

fn _lll_backend_stub(): dict { {"default_method": "ny", "auto_method": "ny", "ny_default": true} }

fn _lll_skipped(str: reason): dict { {"skipped": true, "reason": reason} }

fn _lll_first_profile(any: basis): list {
   _lll_rows(basis) > 0 ? [dot_product(_lll_data(basis).get(0), _lll_data(basis).get(0))] : []
}

fn _lll_best_row_norm_sq(any: basis): any {
   def B = _lll_as_matrix(basis)
   def rows = _lll_rows(B)
   def data = _lll_data(B)
   mut best = nil
   mut i = 0
   while(i < rows){
      def n = dot_product(data.get(i), data.get(i))
      if(best == nil || n < best){ best = n }
      i += 1
   }
   best == nil ? Z(0) : best
}

fn gso_profile(any: basis): dict {
   "Return reusable Gram-Schmidt data for a basis.
   The report exposes b*, mu, squared norms, zero-row count, and timing so
   LLL/BKZ/SVP callers can share one profile instead of recomputing their own
   partial view."
   def t0 = ticks()
   def gs_res = gram_schmidt(basis)
   def b_star = gs_res[0]
   def mu = gs_res[1]
   def n = _lll_rows(basis)
   mut norms = []
   mut zero_rows = 0
   mut min_norm = nil
   mut max_norm = 0
   mut i = 0
   while(i < n){
      def norm = dot_product(b_star[i], b_star[i])
      def zero_norm = norm == 0 || norm == Z(0) || _lll_float(norm) == 0.0
      norms = norms.append(norm)
      zero_rows += zero_norm ? 1 : 0
      min_norm = (!zero_norm && (min_norm == nil || norm < min_norm)) ? norm : min_norm
      if(norm > max_norm){ max_norm = norm }
      i += 1
   }
   {
      "rows": n,
      "cols": _lll_cols(basis),
      "b_star": b_star,
      "mu": mu,
      "norms_sq": norms,
      "profile": norms,
      "zero_rows": zero_rows,
      "rank_estimate": n - zero_rows,
      "min_nonzero_norm_sq": min_norm,
      "max_norm_sq": max_norm,
      "profile_slope": 0.0,
      "gso_recomputes": 1,
      "elapsed_ms": _lll_elapsed_ms(t0)
   }
}

fn gso_report(any: basis): dict {
   "Alias for gso_profile; kept as the public report-style API name."
   gso_profile(basis)
}

fn _lll_quality_report_fast_native(any: basis, any: delta=0.75, any: eta=0.51): any {
   "Fast LLL quality checks for small integer bases.
   This returns only the public quality fields, not public b* vectors, so
   report callers avoid generic Gram-Schmidt vector allocation."
   def B = _lll_as_matrix(basis)
   def n = _lll_rows(B)
   def cols = _lll_cols(B)
   def rows = _lll_fast_native_rows(B, 1000000)
   if(rows == nil){ return nil }
   def t0 = ticks()
   if(n <= 0){
      def empty_gso = _lll_set_fields(dict(12), [
            ["method", "small-int-f64buf-gso"], ["rows", 0], ["cols", cols], ["b_star", []], ["mu", []],
            ["norms_sq", []], ["profile", []], ["zero_rows", 0], ["rank_estimate", 0],
            ["profile_slope", 0.0], ["gso_recomputes", 1], ["elapsed_ms", _lll_elapsed_ms(t0)],
      ])
      def empty = _lll_set_fields(dict(16), [
            ["rows", 0], ["cols", cols], ["delta", delta], ["eta", eta], ["reduced", true], ["is_reduced", true],
            ["ok", true], ["size_reduced", true], ["lovasz", true], ["max_mu", 0.0], ["profile", []],
            ["violations", []], ["numeric_kernel", "small-int-f64buf-gso"], ["quality_fast_path", true],
            ["gso", empty_gso], ["elapsed_ms", _lll_elapsed_ms(t0)],
      ])
      return empty
   }
   def supports = _lll_fast_supports(rows)
   def gso = _lll_fast_gso_prefix_int_support(rows, supports, n - 1)
   def list<ptr>: mu = gso[1]
   def ptr: profile_buf = gso[2]
   def profile = _lll_fast_f64buf_to_list(profile_buf, n)
   mut violations = []
   mut max_mu = 0.0
   mut zero_rows = 0
   mut min_norm = nil
   mut max_norm = 0.0
   mut size_reduced = true
   mut lovasz = true
   def f64: eta_f = float(eta)
   def f64: delta_f = float(delta)
   mut i = 0
   while(i < n){
      def f64: norm_i = f64buf_load(profile_buf, i)
      def zero_norm = norm_i == 0.0
      zero_rows += zero_norm ? 1 : 0
      if(!zero_norm && (min_norm == nil || norm_i < min_norm)){ min_norm = norm_i }
      if(norm_i > max_norm){ max_norm = norm_i }
      mut j = 0
      while(j < i){
         def f64: mu_ij = f64buf_load(mu[i], j)
         def a = abs(mu_ij)
         if(a > max_mu){ max_mu = a }
         if(a > eta_f){
            size_reduced = false
            violations = violations.append({"kind": "size", "i": i, "j": j, "mu": mu_ij, "eta": eta})
         }
         j += 1
      }
      if(i > 0){
         def f64: mu_prev = f64buf_load(mu[i], i - 1)
         def f64: lhs = f64buf_load(profile_buf, i)
         def f64: rhs = (delta_f - mu_prev * mu_prev) * f64buf_load(profile_buf, i - 1)
         if(lhs < rhs){
            lovasz = false
            violations = violations.append({"kind": "lovasz", "i": i, "lhs": lhs, "rhs": rhs, "mu": mu_prev, "delta": delta})
         }
      }
      i += 1
   }
   def gso_out = _lll_set_fields(dict(12), [
         ["method", "small-int-f64buf-gso"], ["rows", n], ["cols", cols],
         ["b_star", _lll_skipped("compact quality report")], ["mu", _lll_fast_mu_bufs_to_lists(mu, n)], ["norms_sq", profile], ["profile", profile],
         ["zero_rows", zero_rows], ["rank_estimate", n - zero_rows], ["min_nonzero_norm_sq", min_norm],
         ["max_norm_sq", max_norm], ["profile_slope", 0.0], ["gso_recomputes", 1], ["elapsed_ms", _lll_elapsed_ms(t0)],
   ])
   _lll_set_fields(dict(16), [
         ["rows", n], ["cols", cols], ["delta", delta], ["eta", eta], ["reduced", size_reduced && lovasz],
         ["is_reduced", size_reduced && lovasz], ["ok", size_reduced && lovasz], ["size_reduced", size_reduced],
         ["lovasz", lovasz], ["max_mu", max_mu], ["profile", profile], ["violations", violations],
         ["numeric_kernel", "small-int-f64buf-gso"], ["quality_fast_path", true], ["gso", gso_out],
         ["elapsed_ms", _lll_elapsed_ms(t0)],
   ])
}

fn _lll_auto_threshold(): int {
   mut threshold = 40
   mut env_min = env("NY_LATTICE_FAST_MIN")
   if(is_str(env_min) && env_min.len > 0){ threshold = atoi(env_min) }
   threshold
}

fn _lll_identity(int: n): any {
   mut rows = []
   mut i = 0
   while(i < n){
      mut row = []
      mut j = 0
      while(j < n){
         row = row.append(i == j ? Z(1) : Z(0))
         j += 1
      }
      rows = rows.append(row)
      i += 1
   }
   Matrix(rows)
}

fn _lll_integer_entry(any: x): bigint {
   if(is_bigint(x)){ return x }
   if(is_int(x)){ return bigint_from_int(x) }
   bigint_from_int(round(x))
}

fn _lll_pow10_z(int: n): bigint {
   mut out = Z(1)
   mut i = 0
   while(i < n){
      out = out * Z(10)
      i += 1
   }
   out
}

fn _lll_round_float_string_to_z(str: raw): bigint {
   mut s = raw
   mut neg = false
   if(s.len > 0 && load8(s, 0) == 45){
      neg = true
      s = str.str_slice(s, 1, s.len)
   }
   def epos0 = str.find(s, "e")
   def epos = epos0 >= 0 ? epos0 : str.find(s, "E")
   if(epos < 0){ return neg ? Z(0) - bigint_from_int(round(0.0 - float(raw))) : bigint_from_int(round(float(raw))) }
   mut exp_s = str.str_slice(s, epos + 1, s.len)
   if(exp_s.len > 0 && load8(exp_s, 0) == 43){ exp_s = str.str_slice(exp_s, 1, exp_s.len) }
   def exp10 = atoi(exp_s)
   def mant = str.str_slice(s, 0, epos)
   mut digits = ""
   mut frac = 0
   mut seen_dot = false
   mut i = 0
   while(i < mant.len){
      def c = load8(mant, i)
      if(c == 46){
         seen_dot = true
      } else if(c >= 48 && c <= 57){
         digits = digits + str.str_slice(mant, i, i + 1)
         if(seen_dot){ frac += 1 }
      }
      i += 1
   }
   mut p = 0
   while(p < digits.len && load8(digits, p) == 48){ p += 1 }
   if(p >= digits.len){ return Z(0) }
   digits = str.str_slice(digits, p, digits.len)
   def shift = exp10 - frac
   if(shift < 0){ return bigint_from_int(round(float(raw))) }
   mut zstr = digits
   i = 0
   while(i < shift){
      zstr = zstr + "0"
      i += 1
   }
   def z = Z(zstr)
   neg ? Z(0) - z : z
}

fn _lll_round_to_z(any: x): bigint {
   if(is_bigint(x)){ return x }
   if(is_int(x)){ return bigint_from_int(x) }
   def ax = abs(x)
   if(ax < 9000000000000000.0){ return bigint_from_int(round(x)) }
   _lll_round_float_string_to_z(to_str(x))
}

fn _lll_row_sub_scaled(any: m, int: k, int: j, any: q): any {
   def qz = _lll_round_to_z(q)
   def rk = matrix_get_row(m, k)
   def rj = matrix_get_row(m, j)
   mut out = []
   mut c = 0
   while(c < _lll_cols(m)){
      out = out.append(_lll_integer_entry(rk[c]) - _lll_integer_entry(rj[c]) * qz)
      c += 1
   }
   matrix_set_row(m, k, out)
}

fn _lll_transform_row_sub_scaled(any: m, int: k, int: j, any: q): any {
   def qz = _lll_round_to_z(q)
   def rk = matrix_get_row(m, k)
   def rj = matrix_get_row(m, j)
   mut out = []
   mut c = 0
   while(c < _lll_cols(m)){
      out = out.append(_lll_integer_entry(rk[c]) - _lll_integer_entry(rj[c]) * qz)
      c += 1
   }
   matrix_set_row(m, k, out)
}

fn _lll_lovasz_holds_gso(any: gs_res, int: k, any: delta): bool {
   def b_star = gs_res[0]
   def mu = gs_res[1]
   def b_k_star = b_star[k]
   def b_k_minus_1_star = b_star[k-1]
   def mu_k_k_minus_1 = _lll_get(mu, k, k-1)
   def lhs = dot_product(b_k_star, b_k_star)
   def rhs = (delta - mu_k_k_minus_1 * mu_k_k_minus_1) * dot_product(b_k_minus_1_star, b_k_minus_1_star)
   lhs >= rhs
}

fn _lll_apply_row_op(any: basis, any: transform, int: k, int: j, any: q): list {
   def q_z = _lll_round_to_z(q)
   def next_basis = _lll_row_sub_scaled(basis, k, j, q_z)
   def next_transform = _lll_transform_row_sub_scaled(transform, k, j, q_z)
   [next_basis, next_transform]
}

fn _lll_fast_rows(any: basis): list {
   def data = _lll_data(basis)
   mut out = list(data.len)
   mut i = 0
   while(i < data.len){
      mut row = list(data[i].len)
      mut j = 0
      while(j < data[i].len){
         row = row.append(_lll_integer_entry(data[i][j]))
         j += 1
      }
      out = out.append(row)
      i += 1
   }
   out
}

fn _lll_fast_identity_rows(int: n): list {
   mut out = list(n)
   mut i = 0
   while(i < n){
      mut row = list(n)
      mut j = 0
      while(j < n){
         row = row.append(i == j ? Z(1) : Z(0))
         j += 1
      }
      out = out.append(row)
      i += 1
   }
   out
}

fn _lll_fast_identity_rows_native(int: n): list {
   mut list: out = list(n)
   __list_set_len(out, n)
   mut i = 0
   while(i < n){
      mut list: row = list(n)
      __list_set_len(row, n)
      mut j = 0
      while(j < n){
         row[j] = i == j ? 1 : 0
         j += 1
      }
      out[i] = row
      i += 1
   }
   out
}

fn _lll_fast_matrix(list: rows, int: cols): any {
   [rows.len, cols, rows]
}

fn _lll_fast_f64buf_rows_within_bound(list<ptr>: rows, int: cols, f64: bound): bool {
   def upto = _lll_i_max(0, cols - 1)
   mut i = 0
   while(i < rows.len){
      def ptr: row = rows[i]
      mut c = 0
      while(c <= upto){
         def f64: v = f64buf_load(row, c)
         if(v > bound || v < 0.0 - bound){ return false }
         c += 1
      }
      i += 1
   }
   true
}

fn _lll_fast_native_rows_from_data(list: data, int: bound): ?list {
   def zbound = Z(bound)
   mut out = list(data.len)
   __list_set_len(out, data.len)
   mut i = 0
   while(i < data.len){
      mut list: row = list(data[i].len)
      __list_set_len(row, data[i].len)
      mut j = 0
      while(j < data[i].len){
         def x = data[i][j]
         if(is_int(x)){
            def xi = int(x)
            if(xi > bound || xi < 0 - bound){ return nil }
            row[j] = xi
         } else {
            def z = Z(x)
            if(z > zbound || z < -zbound){ return nil }
            row[j] = int(z)
         }
         j += 1
      }
      out[i] = row
      i += 1
   }
   out
}

fn _lll_fast_native_rows(any: basis, int: bound): ?list {
   _lll_fast_native_rows_from_data(_lll_data(basis), bound)
}

fn _lll_fast_f64buf_row_int(list<int>: row): ptr {
   def int: n = row.len
   def out = f64buf_new(n)
   mut int: i = 0
   while(i < n){
      f64buf_store(out, i, float(_lll_fast_list_int_at(row, i)))
      i += 1
   }
   out
}

fn _lll_fast_f64buf_row_int_limit(list<int>: row, int: limit): ptr {
   def int: n = row.len
   def int: upto = _lll_i_min(_lll_i_max(-1, limit), n - 1)
   def out = f64buf_new(n)
   mut int: i = 0
   while(i <= upto){
      f64buf_store(out, i, float(_lll_fast_list_int_at(row, i)))
      i += 1
   }
   out
}

fn _lll_fast_f64buf_fill_row_int(ptr: out, list<int>: row): ptr {
   def int: n = row.len
   mut int: i = 0
   while(i < n){
      f64buf_store(out, i, float(_lll_fast_list_int_at(row, i)))
      i += 1
   }
   out
}

fn _lll_fast_f64buf_fill_row_int_limit(ptr: out, list<int>: row, int: limit, int: old_limit): ptr {
   def int: n = row.len
   def int: upto = _lll_i_min(_lll_i_max(-1, limit), n - 1)
   def int: old_upto = _lll_i_min(_lll_i_max(-1, old_limit), n - 1)
   mut int: i = 0
   while(i <= upto){
      f64buf_store(out, i, float(_lll_fast_list_int_at(row, i)))
      i += 1
   }
   while(i <= old_upto){
      f64buf_store(out, i, 0.0)
      i += 1
   }
   out
}

@inline
fn _lll_fast_f64buf_dot_prefix(ptr: a, ptr: b, int: limit): f64 {
   mut f64: s0 = 0.0
   mut f64: s1 = 0.0
   mut f64: s2 = 0.0
   mut f64: s3 = 0.0
   mut int: i = 0
   def int: bulk16 = limit - 15
   while(i <= bulk16){
      s0 += f64buf_load(a, i) * f64buf_load(b, i)
      s1 += f64buf_load(a, i + 1) * f64buf_load(b, i + 1)
      s2 += f64buf_load(a, i + 2) * f64buf_load(b, i + 2)
      s3 += f64buf_load(a, i + 3) * f64buf_load(b, i + 3)
      s0 += f64buf_load(a, i + 4) * f64buf_load(b, i + 4)
      s1 += f64buf_load(a, i + 5) * f64buf_load(b, i + 5)
      s2 += f64buf_load(a, i + 6) * f64buf_load(b, i + 6)
      s3 += f64buf_load(a, i + 7) * f64buf_load(b, i + 7)
      s0 += f64buf_load(a, i + 8) * f64buf_load(b, i + 8)
      s1 += f64buf_load(a, i + 9) * f64buf_load(b, i + 9)
      s2 += f64buf_load(a, i + 10) * f64buf_load(b, i + 10)
      s3 += f64buf_load(a, i + 11) * f64buf_load(b, i + 11)
      s0 += f64buf_load(a, i + 12) * f64buf_load(b, i + 12)
      s1 += f64buf_load(a, i + 13) * f64buf_load(b, i + 13)
      s2 += f64buf_load(a, i + 14) * f64buf_load(b, i + 14)
      s3 += f64buf_load(a, i + 15) * f64buf_load(b, i + 15)
      i += 16
   }
   def int: bulk = limit - 7
   while(i <= bulk){
      s0 += f64buf_load(a, i) * f64buf_load(b, i)
      s1 += f64buf_load(a, i + 1) * f64buf_load(b, i + 1)
      s2 += f64buf_load(a, i + 2) * f64buf_load(b, i + 2)
      s3 += f64buf_load(a, i + 3) * f64buf_load(b, i + 3)
      s0 += f64buf_load(a, i + 4) * f64buf_load(b, i + 4)
      s1 += f64buf_load(a, i + 5) * f64buf_load(b, i + 5)
      s2 += f64buf_load(a, i + 6) * f64buf_load(b, i + 6)
      s3 += f64buf_load(a, i + 7) * f64buf_load(b, i + 7)
      i += 8
   }
   while(i <= limit){
      s0 += f64buf_load(a, i) * f64buf_load(b, i)
      i += 1
   }
   (s0 + s1) + (s2 + s3)
}

@inline
fn _lll_fast_f64buf_store_dot_div_norm_prefix(ptr: out, int: out_i, ptr: a, ptr: b, ptr: norms, int: norm_i, int: limit): ptr {
   mut f64: s0 = 0.0
   mut f64: s1 = 0.0
   mut f64: s2 = 0.0
   mut f64: s3 = 0.0
   mut int: i = 0
   def int: bulk16 = limit - 15
   while(i <= bulk16){
      s0 += f64buf_load(a, i) * f64buf_load(b, i)
      s1 += f64buf_load(a, i + 1) * f64buf_load(b, i + 1)
      s2 += f64buf_load(a, i + 2) * f64buf_load(b, i + 2)
      s3 += f64buf_load(a, i + 3) * f64buf_load(b, i + 3)
      s0 += f64buf_load(a, i + 4) * f64buf_load(b, i + 4)
      s1 += f64buf_load(a, i + 5) * f64buf_load(b, i + 5)
      s2 += f64buf_load(a, i + 6) * f64buf_load(b, i + 6)
      s3 += f64buf_load(a, i + 7) * f64buf_load(b, i + 7)
      s0 += f64buf_load(a, i + 8) * f64buf_load(b, i + 8)
      s1 += f64buf_load(a, i + 9) * f64buf_load(b, i + 9)
      s2 += f64buf_load(a, i + 10) * f64buf_load(b, i + 10)
      s3 += f64buf_load(a, i + 11) * f64buf_load(b, i + 11)
      s0 += f64buf_load(a, i + 12) * f64buf_load(b, i + 12)
      s1 += f64buf_load(a, i + 13) * f64buf_load(b, i + 13)
      s2 += f64buf_load(a, i + 14) * f64buf_load(b, i + 14)
      s3 += f64buf_load(a, i + 15) * f64buf_load(b, i + 15)
      i += 16
   }
   def int: bulk = limit - 7
   while(i <= bulk){
      s0 += f64buf_load(a, i) * f64buf_load(b, i)
      s1 += f64buf_load(a, i + 1) * f64buf_load(b, i + 1)
      s2 += f64buf_load(a, i + 2) * f64buf_load(b, i + 2)
      s3 += f64buf_load(a, i + 3) * f64buf_load(b, i + 3)
      s0 += f64buf_load(a, i + 4) * f64buf_load(b, i + 4)
      s1 += f64buf_load(a, i + 5) * f64buf_load(b, i + 5)
      s2 += f64buf_load(a, i + 6) * f64buf_load(b, i + 6)
      s3 += f64buf_load(a, i + 7) * f64buf_load(b, i + 7)
      i += 8
   }
   while(i <= limit){
      s0 += f64buf_load(a, i) * f64buf_load(b, i)
      i += 1
   }
   f64buf_store(out, out_i, ((s0 + s1) + (s2 + s3)) / f64buf_load(norms, norm_i))
   out
}

@inline
fn _lll_fast_f64buf_store_dot_prefix(ptr: out, int: out_i, ptr: a, ptr: b, int: limit): ptr {
   mut f64: s0 = 0.0
   mut f64: s1 = 0.0
   mut f64: s2 = 0.0
   mut f64: s3 = 0.0
   mut int: i = 0
   def int: bulk16 = limit - 15
   while(i <= bulk16){
      s0 += f64buf_load(a, i) * f64buf_load(b, i)
      s1 += f64buf_load(a, i + 1) * f64buf_load(b, i + 1)
      s2 += f64buf_load(a, i + 2) * f64buf_load(b, i + 2)
      s3 += f64buf_load(a, i + 3) * f64buf_load(b, i + 3)
      s0 += f64buf_load(a, i + 4) * f64buf_load(b, i + 4)
      s1 += f64buf_load(a, i + 5) * f64buf_load(b, i + 5)
      s2 += f64buf_load(a, i + 6) * f64buf_load(b, i + 6)
      s3 += f64buf_load(a, i + 7) * f64buf_load(b, i + 7)
      s0 += f64buf_load(a, i + 8) * f64buf_load(b, i + 8)
      s1 += f64buf_load(a, i + 9) * f64buf_load(b, i + 9)
      s2 += f64buf_load(a, i + 10) * f64buf_load(b, i + 10)
      s3 += f64buf_load(a, i + 11) * f64buf_load(b, i + 11)
      s0 += f64buf_load(a, i + 12) * f64buf_load(b, i + 12)
      s1 += f64buf_load(a, i + 13) * f64buf_load(b, i + 13)
      s2 += f64buf_load(a, i + 14) * f64buf_load(b, i + 14)
      s3 += f64buf_load(a, i + 15) * f64buf_load(b, i + 15)
      i += 16
   }
   def int: bulk = limit - 7
   while(i <= bulk){
      s0 += f64buf_load(a, i) * f64buf_load(b, i)
      s1 += f64buf_load(a, i + 1) * f64buf_load(b, i + 1)
      s2 += f64buf_load(a, i + 2) * f64buf_load(b, i + 2)
      s3 += f64buf_load(a, i + 3) * f64buf_load(b, i + 3)
      s0 += f64buf_load(a, i + 4) * f64buf_load(b, i + 4)
      s1 += f64buf_load(a, i + 5) * f64buf_load(b, i + 5)
      s2 += f64buf_load(a, i + 6) * f64buf_load(b, i + 6)
      s3 += f64buf_load(a, i + 7) * f64buf_load(b, i + 7)
      i += 8
   }
   while(i <= limit){
      s0 += f64buf_load(a, i) * f64buf_load(b, i)
      i += 1
   }
   f64buf_store(out, out_i, (s0 + s1) + (s2 + s3))
   out
}

@inline
fn _lll_fast_f64buf_sub_scaled_prefix(ptr: a, ptr: b, f64: coeff, int: limit): ptr {
   mut int: i = 0
   def int: bulk16 = limit - 15
   while(i <= bulk16){
      f64buf_store(a, i, f64buf_load(a, i) - coeff * f64buf_load(b, i))
      f64buf_store(a, i + 1, f64buf_load(a, i + 1) - coeff * f64buf_load(b, i + 1))
      f64buf_store(a, i + 2, f64buf_load(a, i + 2) - coeff * f64buf_load(b, i + 2))
      f64buf_store(a, i + 3, f64buf_load(a, i + 3) - coeff * f64buf_load(b, i + 3))
      f64buf_store(a, i + 4, f64buf_load(a, i + 4) - coeff * f64buf_load(b, i + 4))
      f64buf_store(a, i + 5, f64buf_load(a, i + 5) - coeff * f64buf_load(b, i + 5))
      f64buf_store(a, i + 6, f64buf_load(a, i + 6) - coeff * f64buf_load(b, i + 6))
      f64buf_store(a, i + 7, f64buf_load(a, i + 7) - coeff * f64buf_load(b, i + 7))
      f64buf_store(a, i + 8, f64buf_load(a, i + 8) - coeff * f64buf_load(b, i + 8))
      f64buf_store(a, i + 9, f64buf_load(a, i + 9) - coeff * f64buf_load(b, i + 9))
      f64buf_store(a, i + 10, f64buf_load(a, i + 10) - coeff * f64buf_load(b, i + 10))
      f64buf_store(a, i + 11, f64buf_load(a, i + 11) - coeff * f64buf_load(b, i + 11))
      f64buf_store(a, i + 12, f64buf_load(a, i + 12) - coeff * f64buf_load(b, i + 12))
      f64buf_store(a, i + 13, f64buf_load(a, i + 13) - coeff * f64buf_load(b, i + 13))
      f64buf_store(a, i + 14, f64buf_load(a, i + 14) - coeff * f64buf_load(b, i + 14))
      f64buf_store(a, i + 15, f64buf_load(a, i + 15) - coeff * f64buf_load(b, i + 15))
      i += 16
   }
   def int: bulk = limit - 7
   while(i <= bulk){
      f64buf_store(a, i, f64buf_load(a, i) - coeff * f64buf_load(b, i))
      f64buf_store(a, i + 1, f64buf_load(a, i + 1) - coeff * f64buf_load(b, i + 1))
      f64buf_store(a, i + 2, f64buf_load(a, i + 2) - coeff * f64buf_load(b, i + 2))
      f64buf_store(a, i + 3, f64buf_load(a, i + 3) - coeff * f64buf_load(b, i + 3))
      f64buf_store(a, i + 4, f64buf_load(a, i + 4) - coeff * f64buf_load(b, i + 4))
      f64buf_store(a, i + 5, f64buf_load(a, i + 5) - coeff * f64buf_load(b, i + 5))
      f64buf_store(a, i + 6, f64buf_load(a, i + 6) - coeff * f64buf_load(b, i + 6))
      f64buf_store(a, i + 7, f64buf_load(a, i + 7) - coeff * f64buf_load(b, i + 7))
      i += 8
   }
   while(i <= limit){
      f64buf_store(a, i, f64buf_load(a, i) - coeff * f64buf_load(b, i))
      i += 1
   }
   a
}

@inline
fn _lll_fast_f64buf_sub_scaled_mu_prefix(ptr: a, ptr: b, ptr: mu_row, int: mu_i, int: limit): ptr {
   def f64: coeff = f64buf_load(mu_row, mu_i)
   mut int: i = 0
   def int: bulk16 = limit - 15
   while(i <= bulk16){
      f64buf_store(a, i, f64buf_load(a, i) - coeff * f64buf_load(b, i))
      f64buf_store(a, i + 1, f64buf_load(a, i + 1) - coeff * f64buf_load(b, i + 1))
      f64buf_store(a, i + 2, f64buf_load(a, i + 2) - coeff * f64buf_load(b, i + 2))
      f64buf_store(a, i + 3, f64buf_load(a, i + 3) - coeff * f64buf_load(b, i + 3))
      f64buf_store(a, i + 4, f64buf_load(a, i + 4) - coeff * f64buf_load(b, i + 4))
      f64buf_store(a, i + 5, f64buf_load(a, i + 5) - coeff * f64buf_load(b, i + 5))
      f64buf_store(a, i + 6, f64buf_load(a, i + 6) - coeff * f64buf_load(b, i + 6))
      f64buf_store(a, i + 7, f64buf_load(a, i + 7) - coeff * f64buf_load(b, i + 7))
      f64buf_store(a, i + 8, f64buf_load(a, i + 8) - coeff * f64buf_load(b, i + 8))
      f64buf_store(a, i + 9, f64buf_load(a, i + 9) - coeff * f64buf_load(b, i + 9))
      f64buf_store(a, i + 10, f64buf_load(a, i + 10) - coeff * f64buf_load(b, i + 10))
      f64buf_store(a, i + 11, f64buf_load(a, i + 11) - coeff * f64buf_load(b, i + 11))
      f64buf_store(a, i + 12, f64buf_load(a, i + 12) - coeff * f64buf_load(b, i + 12))
      f64buf_store(a, i + 13, f64buf_load(a, i + 13) - coeff * f64buf_load(b, i + 13))
      f64buf_store(a, i + 14, f64buf_load(a, i + 14) - coeff * f64buf_load(b, i + 14))
      f64buf_store(a, i + 15, f64buf_load(a, i + 15) - coeff * f64buf_load(b, i + 15))
      i += 16
   }
   def int: bulk = limit - 7
   while(i <= bulk){
      f64buf_store(a, i, f64buf_load(a, i) - coeff * f64buf_load(b, i))
      f64buf_store(a, i + 1, f64buf_load(a, i + 1) - coeff * f64buf_load(b, i + 1))
      f64buf_store(a, i + 2, f64buf_load(a, i + 2) - coeff * f64buf_load(b, i + 2))
      f64buf_store(a, i + 3, f64buf_load(a, i + 3) - coeff * f64buf_load(b, i + 3))
      f64buf_store(a, i + 4, f64buf_load(a, i + 4) - coeff * f64buf_load(b, i + 4))
      f64buf_store(a, i + 5, f64buf_load(a, i + 5) - coeff * f64buf_load(b, i + 5))
      f64buf_store(a, i + 6, f64buf_load(a, i + 6) - coeff * f64buf_load(b, i + 6))
      f64buf_store(a, i + 7, f64buf_load(a, i + 7) - coeff * f64buf_load(b, i + 7))
      i += 8
   }
   while(i <= limit){
      f64buf_store(a, i, f64buf_load(a, i) - coeff * f64buf_load(b, i))
      i += 1
   }
   a
}

fn _lll_fast_f64buf_row_any(list: row): ptr {
   def out = f64buf_new(row.len)
   mut i = 0
   while(i < row.len){
      __bigint_f64buf_store(out, i, row[i])
      i += 1
   }
   out
}

fn _lll_fast_f64buf_fill_row_any(ptr: out, list: row): ptr {
   mut i = 0
   while(i < row.len){
      __bigint_f64buf_store(out, i, row[i])
      i += 1
   }
   out
}

fn _lll_fast_f64buf_rows_any(list: rows): list {
   mut list<ptr>: out = list(rows.len)
   __list_set_len(out, rows.len)
   mut i = 0
   while(i < rows.len){
      out[i] = _lll_fast_f64buf_row_any(rows[i])
      i += 1
   }
   out
}

fn _lll_fast_mu_row(int: n): list<f64> {
   mut list<f64>: out = list(n)
   __list_set_len(out, n)
   if(n > 0){ out[n - 1] = 0.0 }
   out
}

fn _lll_fast_mu_buf(int: n): ptr {
   def out = f64buf_new(n)
   if(n > 0){ f64buf_store(out, n - 1, 0.0) }
   out
}

fn _lll_fast_f64buf_clone_prefix(ptr: row, int: n): ptr {
   def out = f64buf_new(n)
   mut int: i = 0
   while(i < n){
      f64buf_store(out, i, f64buf_load(row, i))
      i += 1
   }
   out
}

fn _lll_fast_f64buf_clone_limit(ptr: row, int: n, int: limit): ptr {
   def out = f64buf_new(n)
   mut int: i = 0
   def int: upto = _lll_i_min(limit, n - 1)
   while(i <= upto){
      f64buf_store(out, i, f64buf_load(row, i))
      i += 1
   }
   out
}

fn _lll_fast_f64buf_fill_from_buf(ptr: out, ptr: row, int: limit): ptr {
   mut int: i = 0
   while(i <= limit){
      f64buf_store(out, i, f64buf_load(row, i))
      i += 1
   }
   out
}

fn _lll_fast_f64buf_fill_from_buf_limit(ptr: out, ptr: row, int: limit, int: old_limit): ptr {
   mut int: i = 0
   def int: upto = _lll_i_max(-1, limit)
   def int: old_upto = _lll_i_max(-1, old_limit)
   while(i <= upto){
      f64buf_store(out, i, f64buf_load(row, i))
      i += 1
   }
   while(i <= old_upto){
      f64buf_store(out, i, 0.0)
      i += 1
   }
   out
}

fn _lll_fast_f64buf_to_list(ptr: values, int: n): list {
   mut list<f64>: out = list(n)
   __list_set_len(out, n)
   mut int: i = 0
   while(i < n){
      out[i] = f64buf_load(values, i)
      i += 1
   }
   out
}

@inline
fn _lll_fast_list_int_at(list<int>: xs, int: i): int {
   (load64_i(xs, 16 + i * 8) - 1) / 2
}

@inline
fn _lll_fast_list_int_set(list<int>: xs, int: i, int: v): list<int> {
   __store_item_fast(xs, i, v)
   xs
}

@inline
fn _lll_fast_ptr_at(list<ptr>: xs, int: i): ptr { __load_item_fast(xs, i) }

@inline
fn _lll_fast_ptr_set(list<ptr>: xs, int: i, ptr: v): list<ptr> {
   __store_item_fast(xs, i, v)
   xs
}

@inline
fn _lll_fast_int_row_at(list<list<int>>: xs, int: i): list<int> { __load_item_fast(xs, i) }

@inline
fn _lll_fast_any_at(list: xs, int: i): any { __load_item_fast(xs, i) }

@inline
fn _lll_fast_any_set(list: xs, int: i, any: v): list {
   __store_item_fast(xs, i, v)
   xs
}

@inline
fn _lll_fast_round_f64_to_int(f64: x): int {
   x < 0.0 ? int(x - 0.5) : int(x + 0.5)
}

@inline
fn _lll_i_min(int: a, int: b): int { a < b ? a : b }

@inline
fn _lll_i_max(int: a, int: b): int { a > b ? a : b }

fn _lll_fast_mu_bufs_to_lists(list<ptr>: mu, int: n): list {
   mut out = []
   mut int: i = 0
   while(i < n){
      mut list<f64>: row = list(i + 1)
      __list_set_len(row, i + 1)
      mut int: j = 0
      while(j <= i){
         row[j] = f64buf_load(mu[i], j)
         j += 1
      }
      out = out.append(row)
      i += 1
   }
   out
}

fn _lll_fast_gso_extend_int_support(list: existing, list<list<int>>: rows, list<int>: supports, int: upto): list {
   def int: n = rows.len
   mut int: limit = upto
   if(limit < 0){ limit = 0 }
   if(limit >= n){ limit = n - 1 }
   mut list<ptr>: bstar = existing[0]
   mut list<ptr>: mu = existing[1]
   def ptr: norms = existing[2]
   mut list<int>: bstar_limits = existing[4]
   mut int: i = int(existing[3])
   while(i <= limit){
      mut int: vi_limit = _lll_fast_list_int_at(supports, i)
      def int: old_limit = i < bstar_limits.len ? _lll_fast_list_int_at(bstar_limits, i) : -1
      def list<int>: row_i = _lll_fast_int_row_at(rows, i)
      mut ptr: v = i < bstar.len ? _lll_fast_f64buf_fill_row_int_limit(_lll_fast_ptr_at(bstar, i), row_i, vi_limit, old_limit) : _lll_fast_f64buf_row_int_limit(row_i, vi_limit)
      mut ptr: mu_row = i < mu.len ? _lll_fast_ptr_at(mu, i) : _lll_fast_mu_buf(i + 1)
      f64buf_store(mu_row, i, 0.0)
      mut int: j = 0
      while(j < i){
         def f64: den = f64buf_load(norms, j)
         def int: bj_limit = _lll_fast_list_int_at(bstar_limits, j)
         def int: dot_limit = _lll_i_min(vi_limit, bj_limit)
         def ptr: bj = _lll_fast_ptr_at(bstar, j)
         if(den > 0.0){
            _lll_fast_f64buf_store_dot_div_norm_prefix(mu_row, j, v, bj, norms, j, dot_limit)
            _lll_fast_f64buf_sub_scaled_mu_prefix(v, bj, mu_row, j, bj_limit)
            if(vi_limit < bj_limit){ vi_limit = bj_limit }
         } else {
            f64buf_store(mu_row, j, 0.0)
         }
         j += 1
      }
      if(i < bstar.len){
         _lll_fast_ptr_set(bstar, i, v)
         _lll_fast_f64buf_store_dot_prefix(norms, i, v, v, vi_limit)
         _lll_fast_ptr_set(mu, i, mu_row)
         _lll_fast_list_int_set(bstar_limits, i, vi_limit)
      } else {
         bstar = bstar.append(v)
         _lll_fast_f64buf_store_dot_prefix(norms, i, v, v, vi_limit)
         mu = mu.append(mu_row)
         bstar_limits = bstar_limits.append(vi_limit)
      }
      i += 1
   }
   [bstar, mu, norms, limit + 1, bstar_limits]
}

fn _lll_fast_gso_prefix_int_support(list<list<int>>: rows, list<int>: supports, int: upto): list {
   def int: n = rows.len
   mut int: limit = upto
   if(limit < 0){ limit = 0 }
   if(limit >= n){ limit = n - 1 }
   mut list<ptr>: bstar = list(limit + 1)
   mut list<ptr>: mu = list(limit + 1)
   def ptr: norms = f64buf_new(n)
   mut list<int>: bstar_limits = list(limit + 1)
   __list_set_len(bstar, limit + 1)
   __list_set_len(mu, limit + 1)
   __list_set_len(bstar_limits, limit + 1)
   mut int: i = 0
   while(i <= limit){
      mut int: vi_limit = _lll_fast_list_int_at(supports, i)
      mut ptr: v = _lll_fast_f64buf_row_int_limit(_lll_fast_int_row_at(rows, i), vi_limit)
      mut ptr: mu_row = _lll_fast_mu_buf(i + 1)
      mut int: j = 0
      while(j < i){
         def f64: den = f64buf_load(norms, j)
         def int: bj_limit = _lll_fast_list_int_at(bstar_limits, j)
         def int: dot_limit = _lll_i_min(vi_limit, bj_limit)
         def ptr: bj = _lll_fast_ptr_at(bstar, j)
         if(den > 0.0){
            _lll_fast_f64buf_store_dot_div_norm_prefix(mu_row, j, v, bj, norms, j, dot_limit)
            _lll_fast_f64buf_sub_scaled_mu_prefix(v, bj, mu_row, j, bj_limit)
            if(vi_limit < bj_limit){ vi_limit = bj_limit }
         } else {
            f64buf_store(mu_row, j, 0.0)
         }
         j += 1
      }
      _lll_fast_ptr_set(bstar, i, v)
      _lll_fast_f64buf_store_dot_prefix(norms, i, v, v, vi_limit)
      _lll_fast_ptr_set(mu, i, mu_row)
      _lll_fast_list_int_set(bstar_limits, i, vi_limit)
      i += 1
   }
   [bstar, mu, norms, limit + 1, bstar_limits]
}

fn _lll_fast_gso_extend_buf_rows(list: existing, list<ptr>: rows, int: cols, list<int>: supports, int: upto): list {
   def int: n = rows.len
   mut int: limit = upto
   if(limit < 0){ limit = 0 }
   if(limit >= n){ limit = n - 1 }
   mut list<ptr>: bstar = existing[0]
   mut list<ptr>: mu = existing[1]
   def ptr: norms = existing[2]
   mut list<int>: bstar_limits = existing[4]
   mut int: i = int(existing[3])
   while(i <= limit){
      mut int: vi_limit = _lll_fast_list_int_at(supports, i)
      def int: old_limit = i < bstar_limits.len ? _lll_fast_list_int_at(bstar_limits, i) : -1
      def ptr: row_i = _lll_fast_ptr_at(rows, i)
      def ptr: v = i < bstar.len ? _lll_fast_f64buf_fill_from_buf_limit(_lll_fast_ptr_at(bstar, i), row_i, vi_limit, old_limit) : _lll_fast_f64buf_clone_limit(row_i, cols, vi_limit)
      mut ptr: mu_row = i < mu.len ? _lll_fast_ptr_at(mu, i) : _lll_fast_mu_buf(i + 1)
      f64buf_store(mu_row, i, 0.0)
      mut int: j = 0
      while(j < i){
         def f64: den = f64buf_load(norms, j)
         def int: bj_limit = _lll_fast_list_int_at(bstar_limits, j)
         def int: dot_limit = _lll_i_min(vi_limit, bj_limit)
         def ptr: bj = _lll_fast_ptr_at(bstar, j)
         if(den > 0.0){
            _lll_fast_f64buf_store_dot_div_norm_prefix(mu_row, j, v, bj, norms, j, dot_limit)
            _lll_fast_f64buf_sub_scaled_mu_prefix(v, bj, mu_row, j, bj_limit)
            if(vi_limit < bj_limit){ vi_limit = bj_limit }
         } else {
            f64buf_store(mu_row, j, 0.0)
         }
         j += 1
      }
      if(i < bstar.len){
         _lll_fast_ptr_set(bstar, i, v)
         _lll_fast_f64buf_store_dot_prefix(norms, i, v, v, vi_limit)
         _lll_fast_ptr_set(mu, i, mu_row)
         _lll_fast_list_int_set(bstar_limits, i, vi_limit)
      } else {
         bstar = bstar.append(v)
         _lll_fast_f64buf_store_dot_prefix(norms, i, v, v, vi_limit)
         mu = mu.append(mu_row)
         bstar_limits = bstar_limits.append(vi_limit)
      }
      i += 1
   }
   [bstar, mu, norms, limit + 1, bstar_limits]
}

fn _lll_fast_gso_prefix_buf_rows(list<ptr>: rows, int: cols, list<int>: supports, int: upto): list {
   def int: n = rows.len
   mut int: limit = upto
   if(limit < 0){ limit = 0 }
   if(limit >= n){ limit = n - 1 }
   mut list<ptr>: bstar = list(limit + 1)
   mut list<ptr>: mu = list(limit + 1)
   def ptr: norms = f64buf_new(n)
   mut list<int>: bstar_limits = list(limit + 1)
   __list_set_len(bstar, limit + 1)
   __list_set_len(mu, limit + 1)
   __list_set_len(bstar_limits, limit + 1)
   mut int: i = 0
   while(i <= limit){
      mut int: vi_limit = _lll_fast_list_int_at(supports, i)
      mut ptr: v = _lll_fast_f64buf_clone_limit(_lll_fast_ptr_at(rows, i), cols, vi_limit)
      mut ptr: mu_row = _lll_fast_mu_buf(i + 1)
      mut int: j = 0
      while(j < i){
         def f64: den = f64buf_load(norms, j)
         def int: bj_limit = _lll_fast_list_int_at(bstar_limits, j)
         def int: dot_limit = _lll_i_min(vi_limit, bj_limit)
         def ptr: bj = _lll_fast_ptr_at(bstar, j)
         if(den > 0.0){
            _lll_fast_f64buf_store_dot_div_norm_prefix(mu_row, j, v, bj, norms, j, dot_limit)
            _lll_fast_f64buf_sub_scaled_mu_prefix(v, bj, mu_row, j, bj_limit)
            if(vi_limit < bj_limit){ vi_limit = bj_limit }
         } else {
            f64buf_store(mu_row, j, 0.0)
         }
         j += 1
      }
      _lll_fast_ptr_set(bstar, i, v)
      _lll_fast_f64buf_store_dot_prefix(norms, i, v, v, vi_limit)
      _lll_fast_ptr_set(mu, i, mu_row)
      _lll_fast_list_int_set(bstar_limits, i, vi_limit)
      i += 1
   }
   [bstar, mu, norms, limit + 1, bstar_limits]
}

fn _lll_fast_row_submul(list: rows, int: k, int: j, any: q): list {
   def qz = (is_int(q) || is_bigint(q)) ? q : _lll_round_to_z(q)
   def rk = _lll_fast_any_at(rows, k)
   def rj = _lll_fast_any_at(rows, j)
   __bigint_row_submul_auto(rk, rj, qz)
   _lll_fast_any_set(rows, k, rk)
   rows
}

fn _lll_fast_row_submul_limit(list: rows, int: k, int: j, any: q, int: limit): list {
   def qz = (is_int(q) || is_bigint(q)) ? q : _lll_round_to_z(q)
   def rk = _lll_fast_any_at(rows, k)
   def rj = _lll_fast_any_at(rows, j)
   __bigint_row_submul(rk, rj, qz, limit)
   _lll_fast_any_set(rows, k, rk)
   rows
}

@inline
fn _lll_fast_row_submul_int_checked_limit(list<list<int>>: rows, int: k, int: j, int: q, int: bound, int: limit): list {
   def list: rk = _lll_fast_int_row_at(rows, k)
   def list: rj = _lll_fast_int_row_at(rows, j)
   mut ok = true
   def int: hi_tag = bound * 2 + 1
   def int: lo_tag = 1 - bound * 2
   mut int: row_max_tag = 0
   mut int: c = 0
   while(c <= limit){
      def int: off = 16 + c * 8
      def int: tagged = load64_i(rk, off) - q * load64_i(rj, off) + q
      def int: av_tag = tagged >= 1 ? tagged - 1 : 1 - tagged
      if(tagged > hi_tag || tagged < lo_tag){ ok = false }
      if(av_tag > row_max_tag){ row_max_tag = av_tag }
      store64_i(rk, tagged, off)
      c += 1
   }
   _lll_fast_any_set(rows, k, rk)
   [rows, ok, row_max_tag / 2]
}

fn _lll_fast_row_submul_int_unchecked_limit(list<list<int>>: rows, int: k, int: j, int: q, int: limit): list {
   def list: rk = _lll_fast_int_row_at(rows, k)
   def list: rj = _lll_fast_int_row_at(rows, j)
   mut int: c = 0
   while(c <= limit){
      def int: off = 16 + c * 8
      store64_i(rk, load64_i(rk, off) - q * load64_i(rj, off) + q, off)
      c += 1
   }
   _lll_fast_any_set(rows, k, rk)
   rows
}

fn _lll_fast_supports(list<list<int>>: rows): list<int> {
   mut list<int>: out = list(rows.len)
   __list_set_len(out, rows.len)
   mut i = 0
   while(i < rows.len){
      def list<int>: row = _lll_fast_int_row_at(rows, i)
      mut j = row.len - 1
      while(j >= 0 && _lll_fast_list_int_at(row, j) == 0){ j -= 1 }
      _lll_fast_list_int_set(out, i, j)
      i += 1
   }
   out
}

fn _lll_fast_is_zero_entry(any: v): bool {
   is_int(v) ? int(v) == 0 : v == Z(0)
}

fn _lll_fast_supports_any(list: rows): list<int> {
   mut list<int>: out = list(rows.len)
   __list_set_len(out, rows.len)
   mut i = 0
   while(i < rows.len){
      def row = _lll_fast_any_at(rows, i)
      mut j = row.len - 1
      while(j >= 0 && _lll_fast_is_zero_entry(row[j])){ j -= 1 }
      _lll_fast_list_int_set(out, i, j)
      i += 1
   }
   out
}

fn _lll_fast_row_abs_max(list: row, int: limit): int {
   mut m = 0
   mut j = 0
   def n = _lll_i_min(limit, row.len - 1)
   while(j <= n){
      def raw = _lll_fast_list_int_at(row, j)
      def v = raw < 0 ? 0 - raw : raw
      if(v > m){ m = v }
      j += 1
   }
   m
}

fn _lll_fast_row_abs_maxes(list<list<int>>: rows): list<int> {
   mut list<int>: out = list(rows.len)
   __list_set_len(out, rows.len)
   mut i = 0
   while(i < rows.len){
      def list<int>: row = _lll_fast_int_row_at(rows, i)
      _lll_fast_list_int_set(out, i, _lll_fast_row_abs_max(row, row.len - 1))
      i += 1
   }
   out
}

fn _lll_fast_trim_support(list<int>: row, int: start): int {
   mut int: j = _lll_i_min(start, row.len - 1)
   while(j >= 0 && _lll_fast_list_int_at(row, j) == 0){ j -= 1 }
   j
}

fn _lll_fast_trim_support_any(list: row, int: start): int {
   mut int: j = _lll_i_min(start, row.len - 1)
   while(j >= 0 && _lll_fast_is_zero_entry(row[j])){ j -= 1 }
   j
}

fn _lll_fast_swap_rows(list: rows, int: i, int: j): list {
   def tmp = _lll_fast_any_at(rows, i)
   _lll_fast_any_set(rows, i, _lll_fast_any_at(rows, j))
   _lll_fast_any_set(rows, j, tmp)
   rows
}

fn _lll_fast_move_row(list: rows, int: from_idx, int: to_idx): list {
   if(from_idx == to_idx){ return rows }
   def item = _lll_fast_any_at(rows, from_idx)
   if(from_idx > to_idx){
      mut i = from_idx
      while(i > to_idx){
         _lll_fast_any_set(rows, i, _lll_fast_any_at(rows, i - 1))
         i -= 1
      }
      _lll_fast_any_set(rows, to_idx, item)
      return rows
   }
   mut i = from_idx
   while(i < to_idx){
      _lll_fast_any_set(rows, i, _lll_fast_any_at(rows, i + 1))
      i += 1
   }
   _lll_fast_any_set(rows, to_idx, item)
   rows
}

fn _lll_fast_lovasz_holds(any: gs, int: k, f64: delta): bool {
   def list: mu = gs[1]
   def list: norms = gs[2]
   def f64: mu_prev = mu[k][k - 1]
   def f64: lhs = norms[k]
   def f64: rhs = (delta - mu_prev * mu_prev) * norms[k - 1]
   lhs >= rhs
}

fn _lll_fast_lovasz_holds_buf(any: gs, int: k, f64: delta): bool {
   def list<ptr>: mu = gs[1]
   def ptr: norms = gs[2]
   def f64: mu_prev = f64buf_load(mu[k], k - 1)
   def f64: lhs = f64buf_load(norms, k)
   def f64: rhs = (delta - mu_prev * mu_prev) * f64buf_load(norms, k - 1)
   lhs >= rhs
}

fn _lll_fast_insertion_index(any: gs, int: k, f64: delta): int {
   def list: mu = gs[1]
   def list: norms = gs[2]
   def list: mu_row = mu[k]
   mut suffix = norms[k]
   mut t = k - 1
   while(t >= 0){
      def f64: mij = mu_row[t]
      suffix = suffix + mij * mij * norms[t]
      if(t < k - 1){
         def f64: threshold = delta * norms[t]
         if(threshold < suffix){ return t + 1 }
      }
      t -= 1
   }
   suffix <= 0.0 ? k - 1 : 0
}

fn _lll_fast_insertion_index_buf(any: gs, int: k, f64: delta): int {
   def list<ptr>: mu = gs[1]
   def ptr: norms = gs[2]
   def ptr: mu_row = mu[k]
   mut f64: suffix = f64buf_load(norms, k)
   mut int: t = k - 1
   while(t >= 0){
      def f64: mij = f64buf_load(mu_row, t)
      suffix = suffix + mij * mij * f64buf_load(norms, t)
      if(t < k - 1){
         def f64: threshold = delta * f64buf_load(norms, t)
         if(threshold < suffix){ return t + 1 }
      }
      t -= 1
   }
   suffix <= 0.0 ? k - 1 : 0
}

fn _lll_fast_gso_truncate(any: gs, int: upto): any {
   if(gs == nil || upto < 0){ return nil }
   def valid = _lll_i_min(upto + 1, gs[0].len)
   if(gs.len > 3){ gs[3] = valid } else { gs = gs.append(valid) }
   gs
}

fn _lll_fast_reduce_row_native(list<list<int>>: rows, list<int>: supports, list<int>: row_abs_maxes, int: k, f64: eta, int: max_passes, int: bound, list: gso_hint): list {
   mut list<list<int>>: work = rows
   mut list<int>: supp = supports
   mut list<int>: maxes = row_abs_maxes
   mut int: steps = 0
   mut ok = true
   def list: gs = gso_hint
   def list<ptr>: mu = gs[1]
   def ptr: mu_k = _lll_fast_ptr_at(mu, k)
   mut ptr: mu_row = mu_k
   mut int: j = k - 1
   while(j >= 0 && steps < max_passes){
      def f64: mu_kj = f64buf_load(mu_row, j)
      if(mu_kj > eta || mu_kj < 0.0 - eta){
         if(mu_kj >= 4096.0 || mu_kj <= 0.0 - 4096.0){ return [work, supp, maxes, steps, false, gs] }
         def int: q = _lll_fast_round_f64_to_int(mu_kj)
         if(q != 0){
            def f64: qf = float(q)
            def int: limit = _lll_i_max(0, _lll_fast_list_int_at(supp, j))
            def int: aq = q < 0 ? 0 - q : q
            def int: max_bound = _lll_fast_list_int_at(maxes, k) + aq * _lll_fast_list_int_at(maxes, j)
            if(max_bound <= bound){
               work = _lll_fast_row_submul_int_unchecked_limit(work, k, j, q, limit)
               _lll_fast_list_int_set(maxes, k, max_bound)
            } else {
               def next = _lll_fast_row_submul_int_checked_limit(work, k, j, q, bound, limit)
               work = next[0]
               if(!next[1]){ ok = false }
               _lll_fast_list_int_set(maxes, k, next[2])
            }
            _lll_fast_list_int_set(supp, k, _lll_fast_trim_support(_lll_fast_int_row_at(work, k), _lll_i_max(_lll_fast_list_int_at(supp, k), limit)))
            f64buf_store(mu_row, j, f64buf_load(mu_row, j) - qf)
            def ptr: mu_j = _lll_fast_ptr_at(mu, j)
            mut int: h = 0
            while(h < j){
               f64buf_store(mu_row, h, f64buf_load(mu_row, h) - qf * f64buf_load(mu_j, h))
               h += 1
            }
            steps += 1
         }
      }
      j -= 1
   }
   [work, supp, maxes, steps, ok, gs]
}

fn _lll_fast_reduce_row_native_transform(list<list<int>>: rows, list<list<int>>: transform, list<int>: supports, list<int>: row_abs_maxes, list<int>: transform_abs_maxes, int: k, f64: eta, int: max_passes, int: bound, list: gso_hint): list {
   mut list<list<int>>: work = rows
   mut list<list<int>>: tr = transform
   mut list<int>: supp = supports
   mut list<int>: maxes = row_abs_maxes
   mut list<int>: tr_maxes = transform_abs_maxes
   mut int: steps = 0
   mut ok = true
   def list: gs = gso_hint
   def list<ptr>: mu = gs[1]
   def ptr: mu_k = _lll_fast_ptr_at(mu, k)
   mut ptr: mu_row = mu_k
   mut int: j = k - 1
   while(j >= 0 && steps < max_passes){
      def f64: mu_kj = f64buf_load(mu_row, j)
      if(mu_kj > eta || mu_kj < 0.0 - eta){
         if(mu_kj >= 4096.0 || mu_kj <= 0.0 - 4096.0){ return [work, tr, supp, maxes, tr_maxes, steps, false, gs] }
         def int: q = _lll_fast_round_f64_to_int(mu_kj)
         if(q != 0){
            def f64: qf = float(q)
            def int: limit = _lll_i_max(0, _lll_fast_list_int_at(supp, j))
            def int: aq = q < 0 ? 0 - q : q
            def int: max_bound = _lll_fast_list_int_at(maxes, k) + aq * _lll_fast_list_int_at(maxes, j)
            if(max_bound <= bound){
               work = _lll_fast_row_submul_int_unchecked_limit(work, k, j, q, limit)
               _lll_fast_list_int_set(maxes, k, max_bound)
            } else {
               def next = _lll_fast_row_submul_int_checked_limit(work, k, j, q, bound, limit)
               work = next[0]
               if(!next[1]){ ok = false }
               _lll_fast_list_int_set(maxes, k, next[2])
            }
            def int: tr_limit = _lll_fast_int_row_at(tr, k).len - 1
            def int: tr_bound = _lll_fast_list_int_at(tr_maxes, k) + aq * _lll_fast_list_int_at(tr_maxes, j)
            if(tr_bound <= bound){
               tr = _lll_fast_row_submul_int_unchecked_limit(tr, k, j, q, tr_limit)
               _lll_fast_list_int_set(tr_maxes, k, tr_bound)
            } else {
               def tr_next = _lll_fast_row_submul_int_checked_limit(tr, k, j, q, bound, tr_limit)
               tr = tr_next[0]
               if(!tr_next[1]){ ok = false }
               _lll_fast_list_int_set(tr_maxes, k, tr_next[2])
            }
            _lll_fast_list_int_set(supp, k, _lll_fast_trim_support(_lll_fast_int_row_at(work, k), _lll_i_max(_lll_fast_list_int_at(supp, k), limit)))
            f64buf_store(mu_row, j, f64buf_load(mu_row, j) - qf)
            def ptr: mu_j = _lll_fast_ptr_at(mu, j)
            mut int: h = 0
            while(h < j){
               f64buf_store(mu_row, h, f64buf_load(mu_row, h) - qf * f64buf_load(mu_j, h))
               h += 1
            }
            steps += 1
         }
      }
      j -= 1
   }
   [work, tr, supp, maxes, tr_maxes, steps, ok, gs]
}

fn _lll_fast_reduce_row_buf_shadow(list: rows, any: transform, list<ptr>: float_rows, list<int>: supports, int: k, f64: eta, int: max_passes, int: cols, list: gso_hint): list {
   mut work = rows
   mut tr = transform
   mut list<ptr>: fwork = float_rows
   mut list<int>: supp = supports
   mut int: steps = 0
   def list: gs = gso_hint
   def list<ptr>: mu = gs[1]
   def ptr: mu_row = _lll_fast_ptr_at(mu, k)
   mut refreshed = false
   mut int: j = k - 1
   while(j >= 0 && steps < max_passes){
      def f64: mu_kj = f64buf_load(mu_row, j)
      if(mu_kj > eta || mu_kj < 0.0 - eta){
         if(mu_kj < 1000000000.0 && mu_kj > 0.0 - 1000000000.0){
            def int: q = _lll_fast_round_f64_to_int(mu_kj)
            if(q != 0){
               def f64: qf = float(q)
               def int: limit = _lll_fast_list_int_at(supp, j)
               if(limit >= 0){
                  work = _lll_fast_row_submul_limit(work, k, j, q, limit)
                  _lll_fast_f64buf_sub_scaled_prefix(_lll_fast_ptr_at(fwork, k), _lll_fast_ptr_at(fwork, j), qf, limit)
                  _lll_fast_list_int_set(supp, k, _lll_fast_trim_support_any(_lll_fast_any_at(work, k), _lll_i_max(_lll_fast_list_int_at(supp, k), limit)))
               }
               if(tr != nil){ tr = _lll_fast_row_submul(tr, k, j, q) }
               refreshed = true
               f64buf_store(mu_row, j, f64buf_load(mu_row, j) - qf)
               def ptr: mu_j = _lll_fast_ptr_at(mu, j)
               mut int: h = 0
               while(h < j){
                  f64buf_store(mu_row, h, f64buf_load(mu_row, h) - qf * f64buf_load(mu_j, h))
                  h += 1
               }
               steps += 1
            }
         } else {
            def q = _lll_round_to_z(mu_kj)
            if(q != Z(0)){
               def f64: qf = _lll_float(q)
               def int: limit = _lll_fast_list_int_at(supp, j)
               if(limit >= 0){
                  work = _lll_fast_row_submul_limit(work, k, j, q, limit)
                  _lll_fast_f64buf_sub_scaled_prefix(_lll_fast_ptr_at(fwork, k), _lll_fast_ptr_at(fwork, j), qf, limit)
                  _lll_fast_list_int_set(supp, k, _lll_fast_trim_support_any(_lll_fast_any_at(work, k), _lll_i_max(_lll_fast_list_int_at(supp, k), limit)))
               }
               if(tr != nil){ tr = _lll_fast_row_submul(tr, k, j, q) }
               refreshed = true
               f64buf_store(mu_row, j, f64buf_load(mu_row, j) - qf)
               def ptr: mu_j = _lll_fast_ptr_at(mu, j)
               mut int: h = 0
               while(h < j){
                  f64buf_store(mu_row, h, f64buf_load(mu_row, h) - qf * f64buf_load(mu_j, h))
                  h += 1
               }
               steps += 1
            }
         }
      }
      j -= 1
   }
   if(refreshed){ _lll_fast_ptr_set(fwork, k, _lll_fast_f64buf_fill_row_any(_lll_fast_ptr_at(fwork, k), _lll_fast_any_at(work, k))) }
   [work, tr, fwork, supp, steps, true, gs]
}

fn _lll_reduce_state_fast_buf_with_budget(any: basis, any: delta, any: eta=0.51, int: requested_total_budget=0, int: requested_row_budget=0, bool: track_transform=false): list {
   def int: n = _lll_rows(basis)
   def int: cols = _lll_cols(basis)
   mut work = _lll_fast_rows(basis)
   mut tr = track_transform ? _lll_fast_identity_rows(n) : nil
   mut list<ptr>: fwork = _lll_fast_f64buf_rows_any(work)
   mut list<int>: supports = _lll_fast_supports_any(work)
   mut int: k = 1
   mut int: steps = 0
   mut ok = true
   mut list: gso_cache = []
   mut int: gso_upto = -1
   mut int: deep_insertions = 0
   mut int: insertion_distance = 0
   mut int: native_probe_next = 4096
   mut native_probe_dead = false
   def f64: delta_f = float(delta)
   def f64: eta_f = float(eta)
   def int: row_budget = requested_row_budget > 0 ? requested_row_budget : _lll_i_max(4, n + cols)
   def int: total_budget = requested_total_budget > 0 ? requested_total_budget : _lll_i_max(64, n * n * cols * 16)
   while(k < n && steps < total_budget){
      if(gso_upto < 0){
         gso_cache = _lll_fast_gso_prefix_buf_rows(fwork, cols, supports, k)
         gso_upto = k
      } elif(gso_upto < k){
         gso_cache = _lll_fast_gso_extend_buf_rows(gso_cache, fwork, cols, supports, k)
         gso_upto = k
      }
      def int: row_limit = _lll_i_min(row_budget, total_budget - steps)
      def row_state = _lll_fast_reduce_row_buf_shadow(work, tr, fwork, supports, k, eta_f, row_limit, cols, gso_cache)
      work = row_state[0]
      tr = row_state[1]
      fwork = row_state[2]
      supports = row_state[3]
      steps += row_state[4]
      if(!track_transform && !native_probe_dead && steps >= native_probe_next && steps < total_budget){
         if(_lll_fast_f64buf_rows_within_bound(fwork, cols, 750000000000000.0)){
            def native_work = _lll_fast_native_rows_from_data(work, 750000000000000)
            if(native_work != nil){
               def int: remaining = total_budget - steps
               def native = _lll_reduce_state_fast_native_work_no_transform_with_budget(native_work, cols, delta, eta, remaining, row_budget, k)
               if(native[3]){
                  return [native[0], nil, steps + native[2], true, "buf-to-int-gso-no-transform", total_budget, deep_insertions + native[6], insertion_distance + native[7]]
               }
               native_probe_dead = true
            }
         }
         native_probe_next = steps + 4096
      }
      if(!row_state[5]){ ok = false }
      if(steps >= total_budget){
         ok = false
         break
      }
      if(_lll_fast_lovasz_holds_buf(row_state[6], k, delta_f)){
         gso_cache = row_state[6]
         gso_upto = k
         k += 1
      } else {
         def int: old_k = k
         def int: insert_at = _lll_fast_insertion_index_buf(row_state[6], old_k, delta_f)
         work = _lll_fast_move_row(work, old_k, insert_at)
         if(tr != nil){ tr = _lll_fast_move_row(tr, old_k, insert_at) }
         fwork = _lll_fast_move_row(fwork, old_k, insert_at)
         supports = _lll_fast_move_row(supports, old_k, insert_at)
         if(insert_at > 0){
            gso_cache = _lll_fast_gso_truncate(row_state[6], insert_at - 1)
            gso_upto = insert_at - 1
         } else {
            gso_cache = []
            gso_upto = -1
         }
         if(old_k - insert_at > 1){
            deep_insertions += 1
            insertion_distance += old_k - insert_at
         }
         k = _lll_i_max(insert_at, 1)
         def int: move_cost = _lll_i_max(1, old_k - insert_at)
         if(steps + move_cost > total_budget){
            ok = false
            break
         }
         steps += move_cost
      }
   }
   if(k < n && steps >= total_budget){ ok = false }
   [_lll_fast_matrix(work, cols), tr == nil ? nil : _lll_fast_matrix(tr, n), steps, ok, track_transform ? "buf-gso-bigint-transform" : "buf-gso-bigint-no-transform", total_budget, deep_insertions, insertion_distance]
}

fn _lll_reduce_state_fast_list_with_budget(any: basis, any: delta, any: eta=0.51, int: requested_total_budget=0, int: requested_row_budget=0, bool: track_transform=true): list {
   _lll_reduce_state_fast_buf_with_budget(basis, delta, eta, requested_total_budget, requested_row_budget, track_transform)
}

fn _lll_reduce_state_fast_native_work_no_transform_with_budget(list: initial_work, int: cols, any: delta, any: eta=0.51, int: requested_total_budget=0, int: requested_row_budget=0, int: start_k=1): list {
   def int: n = initial_work.len
   def int: bound = 750000000000000
   mut list: work = initial_work
   mut list: supports = _lll_fast_supports(work)
   mut list: row_abs_maxes = _lll_fast_row_abs_maxes(work)
   mut int: k = _lll_i_max(1, _lll_i_min(start_k, n - 1))
   mut int: steps = 0
   mut ok = true
   mut list: gso_cache = []
   mut int: gso_upto = -1
   mut int: deep_insertions = 0
   mut int: insertion_distance = 0
   def f64: delta_f = float(delta)
   def f64: eta_f = float(eta)
   def int: row_budget = requested_row_budget > 0 ? requested_row_budget : _lll_i_max(4, n + cols)
   def int: total_budget = requested_total_budget > 0 ? requested_total_budget : _lll_i_max(64, n * n * cols * 16)
   while(k < n && steps < total_budget && ok){
      if(gso_upto < 0){
         gso_cache = _lll_fast_gso_prefix_int_support(work, supports, k)
         gso_upto = k
      } else if(gso_upto < k){
         gso_cache = _lll_fast_gso_extend_int_support(gso_cache, work, supports, k)
         gso_upto = k
      }
      def int: row_limit = _lll_i_min(row_budget, total_budget - steps)
      def row_state = _lll_fast_reduce_row_native(work, supports, row_abs_maxes, k, eta_f, row_limit, bound, gso_cache)
      work = row_state[0]
      supports = row_state[1]
      row_abs_maxes = row_state[2]
      steps += row_state[3]
      if(!row_state[4]){ ok = false }
      if(steps >= total_budget){
         ok = false
         break
      }
      if(ok && _lll_fast_lovasz_holds_buf(row_state[5], k, delta_f)){
         gso_cache = row_state[5]
         gso_upto = k
         k += 1
      } else if(ok){
         def int: old_k = k
         def int: insert_at = _lll_fast_insertion_index_buf(row_state[5], old_k, delta_f)
         work = _lll_fast_move_row(work, old_k, insert_at)
         supports = _lll_fast_move_row(supports, old_k, insert_at)
         row_abs_maxes = _lll_fast_move_row(row_abs_maxes, old_k, insert_at)
         if(insert_at > 0){
            gso_cache = _lll_fast_gso_truncate(row_state[5], insert_at - 1)
            gso_upto = insert_at - 1
         } else {
            gso_cache = []
            gso_upto = -1
         }
         if(old_k - insert_at > 1){
            deep_insertions += 1
            insertion_distance += old_k - insert_at
         }
         k = _lll_i_max(insert_at, 1)
         def int: move_cost = _lll_i_max(1, old_k - insert_at)
         if(steps + move_cost > total_budget){
            ok = false
            break
         }
         steps += move_cost
      }
   }
   if(k < n && steps >= total_budget){ ok = false }
   [_lll_fast_matrix(work, cols), nil, steps, ok, "list-gso-int-no-transform", total_budget, deep_insertions, insertion_distance]
}

fn _lll_reduce_state_fast_native_no_transform_with_budget(any: basis, any: delta, any: eta=0.51, int: requested_total_budget=0, int: requested_row_budget=0): list {
   def int: cols = _lll_cols(basis)
   def int: bound = 750000000000000
   def maybe_work = _lll_fast_native_rows(basis, bound)
   if(maybe_work == nil){ return [basis, nil, 0, false, "int-conversion-failed", requested_total_budget, 0, 0] }
   _lll_reduce_state_fast_native_work_no_transform_with_budget(maybe_work, cols, delta, eta, requested_total_budget, requested_row_budget, 1)
}

fn _lll_is_ternary_pair_row(list: row, int: n): bool {
   if(row.len < 2 * n){ return false }
   mut i = 0
   while(i < 2 * n){
      def v = row[i]
      if(v < -1 || v > 1){ return false }
      i += 1
   }
   true
}

fn _lll_is_ternary_range(list: row, int: start, int: stop): bool {
   if(row.len < stop){ return false }
   mut i = start
   while(i < stop){
      def v = row[i]
      if(v < -1 || v > 1){ return false }
      i += 1
   }
   true
}

fn _lll_is_scaled_ternary_range(list: row, int: start, int: stop, any: scale): bool {
   if(row.len < stop){ return false }
   mut i = start
   while(i < stop){
      def v = row[i]
      if(v % scale != 0){ return false }
      def u = v / scale
      if(u < -1 || u > 1){ return false }
      i += 1
   }
   true
}

fn _lll_is_ntru_key_row(list: row, int: n, any: p): bool {
   if(row.len < 2 * n){ return false }
   (_lll_is_ternary_pair_row(row, n)) ||
   (_lll_is_scaled_ternary_range(row, 0, n, p) && _lll_is_ternary_range(row, n, 2 * n)) ||
   (_lll_is_ternary_range(row, 0, n) && _lll_is_scaled_ternary_range(row, n, 2 * n, p))
}

fn _lll_scan_ternary_pair_rows(list: rows, int: n): list {
   mut out = []
   mut i = 0
   while(i < rows.len){
      if(_lll_is_ternary_pair_row(rows[i], n)){ out = out.append(rows[i]) }
      i += 1
   }
   out
}

fn _lll_scan_ntru_key_rows(list: rows, int: n, any: p): list {
   mut out = []
   mut i = 0
   while(i < rows.len){
      if(_lll_is_ntru_key_row(rows[i], n, p)){ out = out.append(rows[i]) }
      i += 1
   }
   out
}

fn lll_find_ternary_pair_rows(any: basis, int: n, int: step_cap=300000, any: delta=0.99, any: eta=0.51): list {
   "Reduce a small integer lattice and return rows whose first `2*n` entries are ternary.
   The scan runs inside the LLL loop so challenge solvers can stop at the first private-key row."
   def B = _lll_as_matrix(basis)
   def cols = _lll_cols(B)
   def bound = 1000000000000
   def maybe_work = _lll_fast_native_rows(B, bound)
   if(maybe_work == nil){
      def reduced = _lll_pure_reduce(B, delta, eta)
      return _lll_scan_ternary_pair_rows(_lll_data(reduced), n)
   }
   mut list: work = maybe_work
   def rows = work.len
   def initial = _lll_scan_ternary_pair_rows(work, n)
   if(initial.len > 0){ return initial }
   if(rows <= 160 && cols <= 256){
      def reduced = _lll_pure_reduce(B, delta, eta)
      def full = _lll_scan_ternary_pair_rows(_lll_data(reduced), n)
      if(full.len > 0){ return full }
   }
   mut list: supports = _lll_fast_supports(work)
   mut list: row_abs_maxes = _lll_fast_row_abs_maxes(work)
   mut k = 1
   mut steps = 0
   mut list: gso_cache = []
   mut gso_upto = -1
   def f64: delta_f = float(delta)
   def f64: eta_f = float(eta)
   def row_budget = _lll_i_max(4, rows + cols)
   while(k < rows && steps < step_cap){
      if(gso_upto < 0){
         gso_cache = _lll_fast_gso_prefix_int_support(work, supports, k)
         gso_upto = k
      } else if(gso_upto < k){
         gso_cache = _lll_fast_gso_extend_int_support(gso_cache, work, supports, k)
         gso_upto = k
      }
      def row_state = _lll_fast_reduce_row_native(work, supports, row_abs_maxes, k, eta_f, row_budget, bound, gso_cache)
      work = row_state[0]
      supports = row_state[1]
      row_abs_maxes = row_state[2]
      steps += row_state[3]
      if(_lll_fast_lovasz_holds_buf(row_state[5], k, delta_f)){
         gso_cache = row_state[5]
         gso_upto = k
         k += 1
      } else {
         def old_k = k
         def insert_at = _lll_fast_insertion_index_buf(row_state[5], old_k, delta_f)
         work = _lll_fast_move_row(work, old_k, insert_at)
         supports = _lll_fast_move_row(supports, old_k, insert_at)
         row_abs_maxes = _lll_fast_move_row(row_abs_maxes, old_k, insert_at)
         if(insert_at > 0){
            gso_cache = _lll_fast_gso_truncate(row_state[5], insert_at - 1)
            gso_upto = insert_at - 1
         } else {
            gso_cache = []
            gso_upto = -1
         }
         k = _lll_i_max(insert_at, 1)
         steps += old_k - insert_at
      }
   }
   def partial = _lll_scan_ternary_pair_rows(work, n)
   def reduced = _lll_pure_reduce(B, delta, eta)
   def full = _lll_scan_ternary_pair_rows(_lll_data(reduced), n)
   full.len > 0 ? full : partial
}

fn lll_find_ntru_key_rows(any: basis, int: n, any: p, int: step_cap=300000, any: delta=0.99, any: eta=0.51): list {
   "Reduce an NTRU key lattice and return rows shaped like `[f | p*g]`, `[p*g | f]`, or `[f | g]`."
   def B = _lll_as_matrix(basis)
   def cols = _lll_cols(B)
   def bound = 1000000000000
   def maybe_work = _lll_fast_native_rows(B, bound)
   if(maybe_work == nil){
      def reduced = _lll_pure_reduce(B, delta, eta)
      return _lll_scan_ntru_key_rows(_lll_data(reduced), n, p)
   }
   mut list: work = maybe_work
   def rows = work.len
   def initial = _lll_scan_ntru_key_rows(work, n, p)
   if(initial.len > 0){ return initial }
   if(rows <= 160 && cols <= 256){
      def reduced = _lll_pure_reduce(B, delta, eta)
      def full = _lll_scan_ntru_key_rows(_lll_data(reduced), n, p)
      if(full.len > 0){ return full }
   }
   mut list: supports = _lll_fast_supports(work)
   mut list: row_abs_maxes = _lll_fast_row_abs_maxes(work)
   mut k = 1
   mut steps = 0
   mut list: gso_cache = []
   mut gso_upto = -1
   def f64: delta_f = float(delta)
   def f64: eta_f = float(eta)
   def row_budget = _lll_i_max(4, rows + cols)
   while(k < rows && steps < step_cap){
      if(gso_upto < 0){
         gso_cache = _lll_fast_gso_prefix_int_support(work, supports, k)
         gso_upto = k
      } else if(gso_upto < k){
         gso_cache = _lll_fast_gso_extend_int_support(gso_cache, work, supports, k)
         gso_upto = k
      }
      def row_state = _lll_fast_reduce_row_native(work, supports, row_abs_maxes, k, eta_f, row_budget, bound, gso_cache)
      work = row_state[0]
      supports = row_state[1]
      row_abs_maxes = row_state[2]
      steps += row_state[3]
      if(_lll_fast_lovasz_holds_buf(row_state[5], k, delta_f)){
         gso_cache = row_state[5]
         gso_upto = k
         k += 1
      } else {
         def old_k = k
         def insert_at = _lll_fast_insertion_index_buf(row_state[5], old_k, delta_f)
         work = _lll_fast_move_row(work, old_k, insert_at)
         supports = _lll_fast_move_row(supports, old_k, insert_at)
         row_abs_maxes = _lll_fast_move_row(row_abs_maxes, old_k, insert_at)
         if(insert_at > 0){
            gso_cache = _lll_fast_gso_truncate(row_state[5], insert_at - 1)
            gso_upto = insert_at - 1
         } else {
            gso_cache = []
            gso_upto = -1
         }
         k = _lll_i_max(insert_at, 1)
         steps += old_k - insert_at
      }
   }
   def partial = _lll_scan_ntru_key_rows(work, n, p)
   def reduced = _lll_pure_reduce(B, delta, eta)
   def full = _lll_scan_ntru_key_rows(_lll_data(reduced), n, p)
   full.len > 0 ? full : partial
}

fn _lll_reduce_state_fast_native_transform_with_budget(any: basis, any: delta, any: eta=0.51, int: requested_total_budget=0, int: requested_row_budget=0): list {
   def int: n = _lll_rows(basis)
   def int: cols = _lll_cols(basis)
   def int: bound = 1000000000000
   def maybe_work = _lll_fast_native_rows(basis, bound)
   if(maybe_work == nil){ return [basis, nil, 0, false, "int-transform-conversion-failed", requested_total_budget, 0, 0] }
   mut list: work = maybe_work
   mut list: tr = _lll_fast_identity_rows_native(n)
   mut list: supports = _lll_fast_supports(work)
   mut list: row_abs_maxes = _lll_fast_row_abs_maxes(work)
   mut list: tr_abs_maxes = _lll_fast_row_abs_maxes(tr)
   mut int: k = 1
   mut int: steps = 0
   mut ok = true
   mut list: gso_cache = []
   mut gso_upto = -1
   mut deep_insertions = 0
   mut insertion_distance = 0
   def f64: delta_f = float(delta)
   def f64: eta_f = float(eta)
   def int: row_budget = requested_row_budget > 0 ? requested_row_budget : _lll_i_max(4, n + cols)
   def int: total_budget = requested_total_budget > 0 ? requested_total_budget : _lll_i_max(64, n * n * cols * 16)
   while(k < n && steps < total_budget && ok){
      if(gso_upto < 0){
         gso_cache = _lll_fast_gso_prefix_int_support(work, supports, k)
         gso_upto = k
      } else if(gso_upto < k){
         gso_cache = _lll_fast_gso_extend_int_support(gso_cache, work, supports, k)
         gso_upto = k
      }
      def int: row_limit = _lll_i_min(row_budget, total_budget - steps)
      def row_state = _lll_fast_reduce_row_native_transform(work, tr, supports, row_abs_maxes, tr_abs_maxes, k, eta_f, row_limit, bound, gso_cache)
      work = row_state[0]
      tr = row_state[1]
      supports = row_state[2]
      row_abs_maxes = row_state[3]
      tr_abs_maxes = row_state[4]
      steps += row_state[5]
      if(!row_state[6]){ ok = false }
      if(steps >= total_budget){
         ok = false
         break
      }
      if(ok && _lll_fast_lovasz_holds_buf(row_state[7], k, delta_f)){
         gso_cache = row_state[7]
         gso_upto = k
         k += 1
      } else if(ok){
         def old_k = k
         def insert_at = _lll_fast_insertion_index_buf(row_state[7], old_k, delta_f)
         work = _lll_fast_move_row(work, old_k, insert_at)
         tr = _lll_fast_move_row(tr, old_k, insert_at)
         supports = _lll_fast_move_row(supports, old_k, insert_at)
         row_abs_maxes = _lll_fast_move_row(row_abs_maxes, old_k, insert_at)
         tr_abs_maxes = _lll_fast_move_row(tr_abs_maxes, old_k, insert_at)
         if(insert_at > 0){
            gso_cache = _lll_fast_gso_truncate(row_state[7], insert_at - 1)
            gso_upto = insert_at - 1
         } else {
            gso_cache = []
            gso_upto = -1
         }
         if(old_k - insert_at > 1){
            deep_insertions += 1
            insertion_distance += old_k - insert_at
         }
         k = _lll_i_max(insert_at, 1)
         def int: move_cost = _lll_i_max(1, old_k - insert_at)
         if(steps + move_cost > total_budget){
            ok = false
            break
         }
         steps += move_cost
      }
   }
   if(k < n && steps >= total_budget){ ok = false }
   [_lll_fast_matrix(work, cols), _lll_fast_matrix(tr, n), steps, ok, "list-gso-int-transform", total_budget, deep_insertions, insertion_distance]
}

fn _lll_reduce_state_fast_list(any: basis, any: delta, any: eta=0.51): list {
   def state = _lll_reduce_state_fast_list_with_budget(basis, delta, eta)
   [state[0], state[1], state[2], state[3], state[4]]
}

fn _lll_row_violation(any: basis, int: k, any: eta): list {
   def mu_matrix = gram_schmidt(basis)[1]
   mut j = k - 1
   while(j >= 0){
      def mu = _lll_get(mu_matrix, k, j)
      if(abs(mu) > eta){
         return [k, j, _lll_round_to_z(mu)]
      }
      j -= 1
   }
   [-1, -1, 0]
}

fn _lll_babai_row_pass(any: basis, any: transform, int: k, any: eta): list {
   mut work = basis
   mut tr = transform
   mut steps = 0
   mut changed = false
   def gs = gram_schmidt(work)
   def mu_matrix = gs[1]
   mut mu_row = []
   mut i = 0
   while(i <= k){
      mu_row = mu_row.append(_lll_get(mu_matrix, k, i))
      i += 1
   }
   mut j = k - 1
   while(j >= 0){
      def mu_kj = mu_row[j]
      if(abs(mu_kj) > eta){
         def q = _lll_round_to_z(mu_kj)
         if(q != Z(0)){
            def qf = _lll_float(q)
            def next = _lll_apply_row_op(work, tr, k, j, q)
            work = next[0]
            tr = next[1]
            mu_row[j] = mu_row[j] - qf
            mut h = 0
            while(h < j){
               mu_row[h] = mu_row[h] - qf * _lll_get(mu_matrix, j, h)
               h += 1
            }
            steps += 1
            changed = true
         }
      }
      j -= 1
   }
   mut updated_mu = mu_matrix
   mut h2 = 0
   while(h2 <= k){
      updated_mu = _lll_set(updated_mu, k, h2, mu_row[h2])
      h2 += 1
   }
   [work, tr, steps, changed, [gs[0], updated_mu]]
}

fn _lll_reduce_row_babai(any: basis, any: transform, int: k, any: eta, int: max_passes): list {
   mut work = basis
   mut tr = transform
   mut steps = 0
   mut pass = 0
   while(pass < max_passes){
      def state = _lll_babai_row_pass(work, tr, k, eta)
      work = state[0]
      tr = state[1]
      steps += state[2]
      if(!state[3]){ return [work, tr, steps, true, state[4]] }
      pass += 1
   }
   def final_gs = gram_schmidt(work)
   [work, tr, steps, false, final_gs]
}

fn _lll_reduce_row_fixpoint(any: basis, any: transform, int: k, any: eta, int: budget, int: steps): list {
   if(budget <= 0){ return [basis, transform, steps, false] }
   def violation = _lll_row_violation(basis, k, eta)
   if(violation[0] < 0){ return [basis, transform, steps, true] }
   def next = _lll_apply_row_op(basis, transform, violation[0], violation[1], violation[2])
   _lll_reduce_row_fixpoint(next[0], next[1], k, eta, budget - 1, steps + 1)
}

fn _lll_first_size_violation(any: basis, any: eta): list {
   def n = _lll_rows(basis)
   mut i = 1
   while(i < n){
      def violation = _lll_row_violation(basis, i, eta)
      if(violation[0] >= 0){ return violation }
      i += 1
   }
   [-1, -1, 0]
}

fn _lll_size_reduce_fixpoint(any: basis, any: transform, any: eta, int: budget, int: steps): list {
   if(budget <= 0){ return [basis, transform, steps, false] }
   def violation = _lll_first_size_violation(basis, eta)
   if(violation[0] < 0){ return [basis, transform, steps, true] }
   def next = _lll_apply_row_op(basis, transform, violation[0], violation[1], violation[2])
   _lll_size_reduce_fixpoint(next[0], next[1], eta, budget - 1, steps + 1)
}

fn _lll_reduce_row_sweep(any: basis, any: transform, int: k, any: eta, int: max_passes=4): list {
   mut work = basis
   mut tr = transform
   mut steps = 0
   mut pass = 0
   mut changed = true
   while(changed && pass < max_passes){
      changed = false
      def mu = gram_schmidt(work)[1]
      mut j = k - 1
      while(j >= 0){
         def mu_kj = _lll_get(mu, k, j)
         if(abs(mu_kj) > eta){
            def q = _lll_round_to_z(mu_kj)
            if(q != Z(0)){
               def next = _lll_apply_row_op(work, tr, k, j, q)
               work = next[0]
               tr = next[1]
               steps += 1
               changed = true
            }
         }
         j -= 1
      }
      pass += 1
   }
   [work, tr, steps]
}

fn _lll_size_reduce_sweep(any: basis, any: transform, any: eta, int: max_passes=4): list {
   mut work = basis
   mut tr = transform
   mut steps = 0
   mut pass = 0
   mut changed = true
   def n = _lll_rows(basis)
   while(changed && pass < max_passes){
      changed = false
      def mu = gram_schmidt(work)[1]
      mut i = 1
      while(i < n){
         mut j = i - 1
         while(j >= 0){
            def mu_ij = _lll_get(mu, i, j)
            if(abs(mu_ij) > eta){
               def q = _lll_round_to_z(mu_ij)
               if(q != Z(0)){
                  def next = _lll_apply_row_op(work, tr, i, j, q)
                  work = next[0]
                  tr = next[1]
                  steps += 1
                  changed = true
               }
            }
            j -= 1
         }
         i += 1
      }
      pass += 1
   }
   [work, tr, steps]
}

fn _lll_xf_abs(any: x): bigint { x < Z(0) ? Z(0) - x : x }

fn _lll_xf_mul(any: a, any: b, any: scale): bigint { (a * b) / scale }

fn _lll_xf_div(any: a, any: b, any: scale): bigint {
   if(b == Z(0)){ return Z(0) }
   (a * scale) / b
}

fn _lll_xf_from_scalar(any: x, any: scale): bigint {
   if(is_float(x)){
      def ppm = bigint_from_int(round(float(x) * 1000000.0))
      return(ppm * scale) / Z(1000000)
   }
   Z(x) * scale
}

fn _lll_xf_round_to_z(any: x, any: scale): bigint {
   def neg = x < Z(0)
   def a = neg ? Z(0) - x : x
   mut q = a / scale
   def r = a % scale
   if(r * Z(2) >= scale){ q += Z(1) }
   neg ? Z(0) - q : q
}

fn _lll_xf_row(list: row, any: scale): list {
   mut out = []
   mut i = 0
   while(i < row.len){
      out = out.append(_lll_xf_from_scalar(row[i], scale))
      i += 1
   }
   out
}

fn _lll_xf_dot(list: a, list: b, any: scale): bigint {
   mut sum = Z(0)
   mut i = 0
   while(i < a.len){
      sum += _lll_xf_mul(a[i], b[i], scale)
      i += 1
   }
   sum
}

fn _lll_xf_sub_scaled(list: a, list: b, any: s, any: scale): list {
   mut out = []
   mut i = 0
   while(i < a.len){
      out = out.append(a[i] - _lll_xf_mul(b[i], s, scale))
      i += 1
   }
   out
}

fn _lll_xf_gram_schmidt(any: basis, any: scale): list {
   def n = _lll_rows(basis)
   mut b_star = []
   mut mu = matrix_zero(n, n)
   mut norms = []
   def basis_data = _lll_data(basis)
   mut i = 0
   while(i < n){
      mut v = _lll_xf_row(basis_data[i], scale)
      mut j = 0
      while(j < i){
         def bj = b_star[j]
         def den = norms[j]
         def mu_ij = den == Z(0) ? Z(0) : _lll_xf_div(_lll_xf_dot(v, bj, scale), den, scale)
         mu = _lll_set(mu, i, j, mu_ij)
         if(den != Z(0)){ v = _lll_xf_sub_scaled(v, bj, mu_ij, scale) }
         j += 1
      }
      b_star = b_star.append(v)
      norms = norms.append(_lll_xf_dot(v, v, scale))
      i += 1
   }
   [b_star, mu, norms]
}

fn _lll_xf_lovasz_holds_gso(any: gs_res, int: k, any: delta_xf, any: scale): bool {
   def mu = gs_res[1]
   def norms = gs_res[2]
   def lhs = norms[k]
   def mu_prev = _lll_get(mu, k, k - 1)
   def rhs_factor = delta_xf - _lll_xf_mul(mu_prev, mu_prev, scale)
   if(rhs_factor <= Z(0)){ return true }
   lhs >= _lll_xf_mul(rhs_factor, norms[k - 1], scale)
}

fn _lll_xf_babai_row_pass(any: basis, any: transform, int: k, any: eta_xf, any: scale): list {
   mut work = basis
   mut tr = transform
   mut steps = 0
   mut changed = false
   def gs = _lll_xf_gram_schmidt(work, scale)
   def mu_matrix = gs[1]
   mut mu_row = []
   mut i = 0
   while(i <= k){
      mu_row = mu_row.append(_lll_get(mu_matrix, k, i))
      i += 1
   }
   mut j = k - 1
   while(j >= 0){
      def mu_kj = mu_row[j]
      if(_lll_xf_abs(mu_kj) > eta_xf){
         def q = _lll_xf_round_to_z(mu_kj, scale)
         if(q != Z(0)){
            def qxf = q * scale
            def next = _lll_apply_row_op(work, tr, k, j, q)
            work = next[0]
            tr = next[1]
            mu_row[j] = mu_row[j] - qxf
            mut h = 0
            while(h < j){
               mu_row[h] = mu_row[h] - _lll_xf_mul(qxf, _lll_get(mu_matrix, j, h), scale)
               h += 1
            }
            steps += 1
            changed = true
         }
      }
      j -= 1
   }
   [work, tr, steps, changed]
}

fn _lll_xf_reduce_row(any: basis, any: transform, int: k, any: eta_xf, any: scale, int: max_passes): list {
   mut work = basis
   mut tr = transform
   mut steps = 0
   mut pass = 0
   while(pass < max_passes){
      def state = _lll_xf_babai_row_pass(work, tr, k, eta_xf, scale)
      work = state[0]
      tr = state[1]
      steps += state[2]
      if(!state[3]){ return [work, tr, steps, true] }
      pass += 1
   }
   [work, tr, steps, false]
}

fn _lll_xf_size_reduce_sweep(any: basis, any: transform, any: eta_xf, any: scale, int: max_passes=4): list {
   mut work = basis
   mut tr = transform
   mut steps = 0
   mut pass = 0
   mut changed = true
   def n = _lll_rows(basis)
   while(changed && pass < max_passes){
      changed = false
      mut i = 1
      while(i < n){
         def state = _lll_xf_reduce_row(work, tr, i, eta_xf, scale, 4)
         work = state[0]
         tr = state[1]
         steps += state[2]
         if(state[2] > 0){ changed = true }
         i += 1
      }
      pass += 1
   }
   [work, tr, steps, !changed]
}

fn _lll_reduce_state_xfixed(any: basis, any: delta, any: eta=0.51): list {
   def n = _lll_rows(basis)
   def cols = _lll_cols(basis)
   def max_digits = _lll_scale_digits(basis) + 80
   mut precision_digits = max(96, (max_digits / max(1, n)) + 80)
   def scale = _lll_pow10_z(precision_digits)
   mut work = basis
   mut tr = _lll_identity(n)
   mut k = 1
   mut steps = 0
   mut ok = true
   def delta_xf = _lll_xf_from_scalar(delta, scale)
   def eta_xf = _lll_xf_from_scalar(eta, scale)
   def row_budget = max(8, n + cols)
   def total_budget = max(256, n * n * cols * 128)
   while(k < n && steps < total_budget){
      def row_state = _lll_xf_reduce_row(work, tr, k, eta_xf, scale, row_budget)
      work = row_state[0]
      tr = row_state[1]
      steps += row_state[2]
      if(!row_state[3]){ ok = false }
      def gs = _lll_xf_gram_schmidt(work, scale)
      if(_lll_xf_lovasz_holds_gso(gs, k, delta_xf, scale)){
         k += 1
      } else {
         def old_k = k
         work = matrix_swap_rows(work, old_k, old_k - 1)
         tr = matrix_swap_rows(tr, old_k, old_k - 1)
         k = max(old_k - 1, 1)
         steps += 1
      }
   }
   if(k < n && steps >= total_budget){ ok = false }
   def final_state = _lll_xf_size_reduce_sweep(work, tr, eta_xf, scale, 6)
   work = final_state[0]
   tr = final_state[1]
   steps += final_state[2]
   if(!final_state[3]){ ok = false }
   [work, tr, steps, ok, "adaptive-fixed-gso-transform", precision_digits]
}

fn _lll_high_dynamic_small_basis(any: basis): bool {
   _lll_scale_digits(basis) > 0 && _lll_rows(basis) <= 16 && _lll_cols(basis) <= 32
}

fn _lll_reduce_state_lazy(any: basis, any: delta, any: eta=0.51): list {
   def n = _lll_rows(basis)
   def cols = _lll_cols(basis)
   mut work = basis
   mut tr = _lll_identity(n)
   mut k = 1
   mut steps = 0
   mut ok = true
   def total_budget = max(64, n * n * cols * 32)
   while(k < n && steps < total_budget){
      def sweep = _lll_reduce_row_sweep(work, tr, k, eta, 4)
      work = sweep[0]
      tr = sweep[1]
      steps += sweep[2]
      def gs = gram_schmidt(work)
      if(_lll_lovasz_holds_gso(gs, k, delta)){
         k += 1
      } else {
         def old_k = k
         work = matrix_swap_rows(work, old_k, old_k - 1)
         tr = matrix_swap_rows(tr, old_k, old_k - 1)
         k = max(old_k - 1, 1)
         steps += 1
      }
   }
   if(k < n && steps >= total_budget){ ok = false }
   def final_sweep = _lll_size_reduce_sweep(work, tr, eta, 6)
   work = final_sweep[0]
   tr = final_sweep[1]
   steps += final_sweep[2]
   [work, tr, steps, ok]
}

fn _lll_reduce_state_strict(any: basis, any: delta, any: eta=0.51): list {
   def n = _lll_rows(basis)
   def cols = _lll_cols(basis)
   if(n > 32 || cols > 64){
      def quality = lll_quality_report(basis, delta, eta)
      if(quality.get("reduced", false)){
         return [basis, _lll_identity(n), 0, true, "quality-prechecked-identity-transform"]
      }
   }
   mut work = basis
   mut tr = _lll_identity(n)
   mut k = 1
   mut steps = 0
   mut ok = true
   def row_budget = _lll_i_max(4, n + cols)
   def total_budget = _lll_i_max(64, n * n * cols * 16)
   while(k < n && steps < total_budget){
      def row_state = _lll_reduce_row_babai(work, tr, k, eta, row_budget)
      work = row_state[0]
      tr = row_state[1]
      steps += row_state[2]
      if(!row_state[3]){ ok = false }
      def row_gs = row_state.len > 4 ? row_state[4] : gram_schmidt(work)
      if(_lll_lovasz_holds_gso(row_gs, k, delta)){
         k += 1
      } else {
         def old_k = k
         work = matrix_swap_rows(work, old_k, old_k - 1)
         tr = matrix_swap_rows(tr, old_k, old_k - 1)
         k = max(old_k - 1, 1)
         steps += 1
      }
   }
   if(k < n && steps >= total_budget){ ok = false }
   def final_budget = _lll_i_max(64, n * n * cols * 16)
   def final_state = _lll_size_reduce_fixpoint(work, tr, eta, final_budget, 0)
   work = final_state[0]
   tr = final_state[1]
   steps += final_state[2]
   if(!final_state[3]){ ok = false }
   [work, tr, steps, ok]
}

fn _lll_pure_reduce(any: basis, any: delta, any: eta=0.51): any {
   def B = _lll_as_matrix(basis)
   def rows = _lll_rows(B)
   def cols = _lll_cols(B)
   if(rows <= 160 && cols <= 256 && _lll_scale_digits(B) == 0){
      def native = _lll_reduce_state_fast_native_no_transform_with_budget(B, delta, eta, 0, 0)
      if(native[3] && lll_quality_report(native[0], delta, eta).get("reduced", false)){ return native[0] }
      def fast_list = _lll_reduce_state_fast_list_with_budget(B, delta, eta, 0, 0, false)
      if(fast_list[3] && lll_quality_report(fast_list[0], delta, eta).get("reduced", false)){ return fast_list[0] }
   }
   _lll_reduce_state_final(B, delta, eta)[0]
}

fn _lll_reduce_state_final(any: basis, any: delta, any: eta=0.51): list {
   if(_lll_rows(basis) <= 160 && _lll_cols(basis) <= 256 && _lll_scale_digits(basis) == 0){
      def native = _lll_reduce_state_fast_native_transform_with_budget(basis, delta, eta, 0, 0)
      if(native[3] && lll_quality_report(native[0], delta, eta).get("reduced", false)){ return native }
      def fast_list = _lll_reduce_state_fast_list(basis, delta, eta)
      if(fast_list[3] && lll_quality_report(fast_list[0], delta, eta).get("reduced", false)){ return fast_list }
   }
   if(_lll_high_dynamic_small_basis(basis)){
      def xf = _lll_reduce_state_xfixed(basis, delta, eta)
      if(lll_quality_report(xf[0], delta, eta).get("reduced", false)){ return xf }
   }
   def lazy_env = env("NY_LLL_LAZY_SWEEP")
   if(is_str(lazy_env) && (lazy_env == "1" || lazy_env == "true" || lazy_env == "yes") && (_lll_rows(basis) > 8 || _lll_cols(basis) > 8)){
      def lazy = _lll_reduce_state_lazy(basis, delta, eta)
      if(lazy[3] && lll_quality_report(lazy[0], delta, eta).get("reduced", false)){ return lazy }
   }
   _lll_reduce_state_strict(basis, delta, eta)
}

fn _lll_reduce_state_report_fast(any: basis, any: delta, any: eta=0.51, bool: track_transform=true): list {
   if(_lll_rows(basis) <= 160 && _lll_cols(basis) <= 256 && _lll_scale_digits(basis) == 0){
      def native = track_transform ? _lll_reduce_state_fast_native_transform_with_budget(basis, delta, eta, 0, 0) : _lll_reduce_state_fast_native_no_transform_with_budget(basis, delta, eta, 0, 0)
      if(native[3]){ return native }
      def fast_list = track_transform ? _lll_reduce_state_fast_list(basis, delta, eta) : _lll_reduce_state_fast_list_with_budget(basis, delta, eta, 0, 0, false)
      if(fast_list[3]){ return fast_list }
   }
   _lll_reduce_state_final(basis, delta, eta)
}

fn _lll_apply_transform(any: transform, any: basis): any {
   def rows = _lll_rows(transform)
   def inner = _lll_cols(transform)
   def cols = _lll_cols(basis)
   mut out = []
   mut i = 0
   while(i < rows){
      mut row = []
      mut j = 0
      while(j < cols){
         mut s = Z(0)
         mut k = 0
         while(k < inner){
            s += _lll_get(transform, i, k) * _lll_get(basis, k, j)
            k += 1
         }
         row = row.append(s)
         j += 1
      }
      out = out.append(row)
      i += 1
   }
   Matrix(out)
}

fn _lll_transform_verified_fast(any: transform, any: basis, any: reduced): any {
   def bound = 1000000
   def tr = _lll_fast_native_rows(transform, bound)
   def br = _lll_fast_native_rows(basis, bound)
   def rr = _lll_fast_native_rows(reduced, 1000000000000)
   if(tr == nil || br == nil || rr == nil){ return nil }
   def rows = tr.len
   def inner = br.len
   if(rows != rr.len || inner == 0){ return false }
   def cols = br[0].len
   if(rr.len > 0 && rr[0].len != cols){ return false }
   mut i = 0
   while(i < rows){
      def list<int>: ti = tr[i]
      def list<int>: ri = rr[i]
      mut j = 0
      while(j < cols){
         mut int: s = 0
         mut k = 0
         while(k < inner){
            s += _lll_fast_list_int_at(ti, k) * _lll_fast_list_int_at(br[k], j)
            k += 1
         }
         if(s != _lll_fast_list_int_at(ri, j)){ return false }
         j += 1
      }
      i += 1
   }
   true
}

fn _lll_replace_col(any: m, int: col, list: rhs): any {
   def rows = _lll_rows(m)
   def cols = _lll_cols(m)
   mut out = []
   mut i = 0
   while(i < rows){
      mut row = []
      mut j = 0
      while(j < cols){
         row = row.append(j == col ? rhs[i] : _lll_get(m, i, j))
         j += 1
      }
      out = out.append(row)
      i += 1
   }
   Matrix(out)
}

fn _lll_recover_transform(any: basis, any: reduced): any {
   def rows = _lll_rows(basis)
   def cols = _lll_cols(basis)
   if(rows != cols || _lll_rows(reduced) != rows || _lll_cols(reduced) != cols || rows > 8){ return nil }
   def A = matrix_transpose(basis)
   def detA = matrix_det(A)
   if(detA == Z(0) || detA == 0){ return nil }
   mut tr_rows = []
   mut r = 0
   while(r < rows){
      mut rhs = []
      mut j = 0
      while(j < cols){
         rhs = rhs.append(_lll_get(reduced, r, j))
         j += 1
      }
      mut coeffs = []
      j = 0
      while(j < cols){
         def detJ = matrix_det(_lll_replace_col(A, j, rhs))
         coeffs = coeffs.append(detJ / detA)
         j += 1
      }
      tr_rows = tr_rows.append(coeffs)
      r += 1
   }
   Matrix(tr_rows)
}

fn _lll_same_matrix(any: a, any: b): bool {
   if(_lll_rows(a) != _lll_rows(b) || _lll_cols(a) != _lll_cols(b)){ return false }
   mut i = 0
   while(i < _lll_rows(a)){
      mut j = 0
      while(j < _lll_cols(a)){
         if(_lll_get(a, i, j) != _lll_get(b, i, j)){ return false }
         j += 1
      }
      i += 1
   }
   true
}

fn _lll_as_matrix(any: m): any { is_matrix(m) ? m : Matrix(m) }

fn _lll_gram_square(any: gram): any {
   def G = _lll_as_matrix(gram)
   if(_lll_rows(G) != _lll_cols(G)){ panic("lll_gram: Gram matrix must be square") }
   G
}

fn _lll_gram_row_sub_scaled(any: gram, int: k, int: j, any: q): any {
   def qz = _lll_round_to_z(q)
   if(qz == Z(0)){ return gram }
   def n = _lll_rows(gram)
   def old_kk = _lll_get(gram, k, k)
   def old_kj = _lll_get(gram, k, j)
   def old_jj = _lll_get(gram, j, j)
   mut rows = []
   mut r = 0
   while(r < n){
      mut row = []
      mut c = 0
      while(c < n){
         mut v = _lll_get(gram, r, c)
         if(r == k && c == k){
            v = old_kk - Z(2) * qz * old_kj + qz * qz * old_jj
         } elif(r == k){
            v = _lll_get(gram, k, c) - qz * _lll_get(gram, j, c)
         } elif(c == k){
            v = _lll_get(gram, r, k) - qz * _lll_get(gram, r, j)
         }
         row = row.append(v)
         c += 1
      }
      rows = rows.append(row)
      r += 1
   }
   Matrix(rows)
}

fn _lll_gram_swap(any: gram, int: a, int: b): any {
   if(a == b){ return gram }
   def n = _lll_rows(gram)
   mut rows = []
   mut r = 0
   while(r < n){
      def rr = r == a ? b : (r == b ? a : r)
      mut row = []
      mut c = 0
      while(c < n){
         def cc = c == a ? b : (c == b ? a : c)
         row = row.append(_lll_get(gram, rr, cc))
         c += 1
      }
      rows = rows.append(row)
      r += 1
   }
   Matrix(rows)
}

fn _lll_basis_gram(any: basis): any {
   def B = _lll_as_matrix(basis)
   def rows = _lll_rows(B)
   def cols = _lll_cols(B)
   mut out = []
   mut r = 0
   while(r < rows){
      mut row = []
      mut c = 0
      while(c < rows){
         mut s = Z(0)
         mut k = 0
         while(k < cols){
            s += _lll_get(B, r, k) * _lll_get(B, c, k)
            k += 1
         }
         row = row.append(s)
         c += 1
      }
      out = out.append(row)
      r += 1
   }
   Matrix(out)
}

fn _lll_basis_swap(any: basis, int: a, int: b): any {
   if(a == b){ return basis }
   mut data = _lll_data(basis)
   def tmp = data[a]
   data[a] = data[b]
   data[b] = tmp
   Matrix(data)
}

fn _lll_basis_move(any: basis, int: from_idx, int: to_idx): any {
   if(from_idx == to_idx){ return basis }
   mut data = _lll_data(basis)
   data = _lll_fast_move_row(data, from_idx, to_idx)
   Matrix(data)
}

fn _lll_basis_row_add_scaled(any: basis, int: dst, int: src, any: q): any {
   def qz = _lll_round_to_z(q)
   if(qz == Z(0)){ return basis }
   mut data = _lll_data(basis)
   mut row = data[dst]
   def src_row = data[src]
   mut c = 0
   while(c < row.len){
      row[c] = Z(row[c]) + qz * Z(src_row[c])
      c += 1
   }
   data[dst] = row
   Matrix(data)
}

fn _lll_gram_move(any: gram, int: from_idx, int: to_idx): any {
   if(from_idx == to_idx){ return gram }
   def n = _lll_rows(gram)
   mut rows = []
   mut r = 0
   while(r < n){
      mut row = []
      mut c = 0
      while(c < n){
         row = row.append(_lll_get(gram, r, c))
         c += 1
      }
      rows = rows.append(row)
      r += 1
   }
   rows = _lll_fast_move_row(rows, from_idx, to_idx)
   r = 0
   while(r < rows.len){
      rows[r] = _lll_fast_move_row(rows[r], from_idx, to_idx)
      r += 1
   }
   Matrix(rows)
}

fn _lll_parity_op_kind(any: op): str {
   if(is_dict(op)){ return op.get("kind", op.get("op", "")) }
   is_list(op) && op.len > 0 ? to_str(op[0]) : ""
}

fn _lll_parity_op_i(any: op, str: key, int: pos, int: fallback=0): int {
   if(is_dict(op)){ return int(op.get(key, fallback)) }
   is_list(op) && op.len > pos ? int(op[pos]) : fallback
}

fn _lll_parity_op_q(any: op, any: fallback=1): any {
   if(is_dict(op)){ return op.get("q", op.get("coeff", fallback)) }
   is_list(op) && op.len > 3 ? op[3] : fallback
}

fn _lll_apply_gso_parity_op(any: basis, any: gram, any: op): list {
   def kind = _lll_parity_op_kind(op)
   if(kind == "swap"){
      def i = _lll_parity_op_i(op, "i", 1)
      def j = _lll_parity_op_i(op, "j", 2)
      return [_lll_basis_swap(basis, i, j), _lll_gram_swap(gram, i, j)]
   }
   if(kind == "move"){
      def from_idx = _lll_parity_op_i(op, "from", 1)
      def to_idx = _lll_parity_op_i(op, "to", 2)
      return [_lll_basis_move(basis, from_idx, to_idx), _lll_gram_move(gram, from_idx, to_idx)]
   }
   if(kind == "row_add"){
      def i = _lll_parity_op_i(op, "i", 1)
      def j = _lll_parity_op_i(op, "j", 2)
      def q = _lll_parity_op_q(op, 1)
      return [_lll_basis_row_add_scaled(basis, i, j, q), _lll_gram_row_sub_scaled(gram, i, j, Z(0) - _lll_round_to_z(q))]
   }
   if(kind == "row_sub"){
      def i = _lll_parity_op_i(op, "i", 1)
      def j = _lll_parity_op_i(op, "j", 2)
      def q = _lll_parity_op_q(op, 1)
      return [_lll_basis_row_add_scaled(basis, i, j, Z(0) - _lll_round_to_z(q)), _lll_gram_row_sub_scaled(gram, i, j, q)]
   }
   [basis, gram]
}

fn _lll_rel_diff(any: a, any: b): f64 {
   def af = float(a)
   def bf = float(b)
   def den = abs(af) + abs(bf)
   den == 0.0 ? abs(af - bf) : abs(af - bf) / den
}

fn _lll_gso_parity_diff(dict: basis_gso, dict: gram_gso): dict {
   def rows = int(basis_gso.get("rows", 0))
   def bmu = basis_gso.get("mu")
   def gmu = gram_gso.get("mu")
   def bnorms = basis_gso.get("norms_sq", [])
   def gnorms = gram_gso.get("norms_sq", [])
   mut max_mu_abs, max_mu_relative, max_norm_relative = 0.0, 0.0, 0.0
   mut worst_mu, worst_norm = nil, nil
   mut i = 0
   while(i < rows){
      mut j = 0
      while(j < i){
         def bm = _lll_get(bmu, i, j)
         def gm = _lll_get(gmu, i, j)
         def ad = abs(float(bm) - float(gm))
         def rd = _lll_rel_diff(bm, gm)
         if(ad > max_mu_abs){
            max_mu_abs = ad
            worst_mu = {"i": i, "j": j, "basis_mu": bm, "gram_mu": gm}
         }
         if(rd > max_mu_relative){ max_mu_relative = rd }
         j += 1
      }
      if(i < bnorms.len && i < gnorms.len){
         def nd = _lll_rel_diff(bnorms[i], gnorms[i])
         if(nd > max_norm_relative){
            max_norm_relative = nd
            worst_norm = {"i": i, "basis_norm": bnorms[i], "gram_norm": gnorms[i]}
         }
      }
      i += 1
   }
   {
      "max_mu_abs_diff": max_mu_abs,
      "max_mu_relative_diff": max_mu_relative,
      "max_norm_relative_diff": max_norm_relative,
      "worst_mu": worst_mu,
      "worst_norm": worst_norm,
   }
}

fn _lll_rat_abs_z(any: x): bigint {
   def z = Z(x)
   z < Z(0) ? (Z(0) - z) : z
}

fn _lll_rat(any: num, any: den=Z(1)): list {
   mut n = Z(num)
   mut d = Z(den)
   if(d == Z(0)){ return [Z(0), Z(1)] }
   if(d < Z(0)){
      n = Z(0) - n
      d = Z(0) - d
   }
   if(n == Z(0)){ return [Z(0), Z(1)] }
   def g = gcd(_lll_rat_abs_z(n), d)
   [n / g, d / g]
}

fn _lll_rat_zero(): list { [Z(0), Z(1)] }

fn _lll_rat_from(any: x): list {
   is_list(x) && x.len >= 2 ? _lll_rat(x[0], x[1]) : _lll_rat(x, Z(1))
}

fn _lll_rat_sub(any: a, any: b): list {
   def x = _lll_rat_from(a)
   def y = _lll_rat_from(b)
   _lll_rat(x[0] * y[1] - y[0] * x[1], x[1] * y[1])
}

fn _lll_rat_mul(any: a, any: b): list {
   def x = _lll_rat_from(a)
   def y = _lll_rat_from(b)
   _lll_rat(x[0] * y[0], x[1] * y[1])
}

fn _lll_rat_div(any: a, any: b): list {
   def x = _lll_rat_from(a)
   def y = _lll_rat_from(b)
   if(y[0] == Z(0)){ return _lll_rat_zero() }
   _lll_rat(x[0] * y[1], x[1] * y[0])
}

fn _lll_rat_square(any: a): list {
   def x = _lll_rat_from(a)
   _lll_rat(x[0] * x[0], x[1] * x[1])
}

fn _lll_rat_round_to_z(any: a): bigint {
   def x = _lll_rat_from(a)
   def neg = x[0] < Z(0)
   def n = neg ? (Z(0) - x[0]) : x[0]
   def d = x[1]
   mut q = n / d
   def r = n % d
   if(r * Z(2) >= d){ q += Z(1) }
   neg ? (Z(0) - q) : q
}

fn _lll_rat_to_float(any: a): f64 {
   def x = _lll_rat_from(a)
   def nd = _lll_entry_digits(x[0])
   def dd = _lll_entry_digits(x[1])
   def scale = max(0, max(nd, dd) - 150)
   def den = _lll_float_scaled(x[1], scale)
   den == 0.0 ? 0.0 : _lll_float_scaled(x[0], scale) / den
}

fn _lll_rat_abs_gt_float(any: a, any: bound): bool {
   abs(_lll_rat_to_float(a)) > float(bound)
}

fn _lll_rat_zero_rows(int: n): list {
   mut rows = []
   mut i = 0
   while(i < n){
      mut row = []
      mut j = 0
      while(j < n){
         row = row.append(_lll_rat_zero())
         j += 1
      }
      rows = rows.append(row)
      i += 1
   }
   rows
}

fn _lll_gram_gso_report_exact(any: G): dict {
   def t0 = ticks()
   def n = _lll_rows(G)
   def scale_digits = _lll_scale_digits(G)
   mut mu_exact = _lll_rat_zero_rows(n)
   mut norms_exact = []
   mut i = 0
   while(i < n){
      mut j = 0
      while(j < i){
         mut s = _lll_rat(_lll_get(G, i, j), Z(1))
         mut h = 0
         while(h < j){
            s = _lll_rat_sub(s, _lll_rat_mul(_lll_rat_mul(mu_exact[i][h], mu_exact[j][h]), norms_exact[h]))
            h += 1
         }
         def den = norms_exact[j]
         mu_exact[i][j] = den[0] == Z(0) ? _lll_rat_zero() : _lll_rat_div(s, den)
         j += 1
      }
      mut norm = _lll_rat(_lll_get(G, i, i), Z(1))
      j = 0
      while(j < i){
         norm = _lll_rat_sub(norm, _lll_rat_mul(_lll_rat_square(mu_exact[i][j]), norms_exact[j]))
         j += 1
      }
      norms_exact = norms_exact.append(norm)
      i += 1
   }
   mut mu = matrix_zero(n, n)
   mut norms = []
   i = 0
   while(i < n){
      mut j = 0
      while(j < i){
         mu = _lll_set(mu, i, j, _lll_rat_to_float(mu_exact[i][j]))
         j += 1
      }
      norms = norms.append(_lll_rat_to_float(norms_exact[i]))
      i += 1
   }
   _lll_set_fields(dict(14), [
         ["method", "gram-gso-exact-rational"], ["rows", n], ["cols", _lll_cols(G)], ["mu", mu],
         ["norms_sq", norms], ["profile", norms], ["mu_exact", mu_exact], ["norms_exact", norms_exact],
         ["exact", true], ["scale_digits", scale_digits], ["elapsed_ms", _lll_elapsed_ms(t0)],
   ])
}

fn _lll_gram_gso_report_float(any: gram): dict {
   def t0 = ticks()
   def G = _lll_gram_square(gram)
   def n = _lll_rows(G)
   def scale_digits = _lll_scale_digits(G)
   mut mu = matrix_zero(n, n)
   mut norms = []
   mut i = 0
   while(i < n){
      mut j = 0
      while(j < i){
         mut s = _lll_float_scaled(_lll_get(G, i, j), scale_digits)
         mut h = 0
         while(h < j){
            s -= float(_lll_get(mu, i, h)) * float(_lll_get(mu, j, h)) * float(norms[h])
            h += 1
         }
         def den = float(norms[j])
         def mij = den == 0.0 ? 0.0 : s / den
         mu = _lll_set(mu, i, j, mij)
         j += 1
      }
      mut norm = _lll_float_scaled(_lll_get(G, i, i), scale_digits)
      j = 0
      while(j < i){
         def mij = float(_lll_get(mu, i, j))
         norm -= mij * mij * float(norms[j])
         j += 1
      }
      if(abs(norm) < 0.000000000001){ norm = 0.0 }
      norms = norms.append(norm)
      i += 1
   }
   _lll_set_fields(dict(10), [
         ["method", "gram-gso"], ["rows", n], ["cols", _lll_cols(G)], ["mu", mu], ["norms_sq", norms],
         ["profile", norms], ["scale_digits", scale_digits], ["elapsed_ms", _lll_elapsed_ms(t0)],
   ])
}

fn _lll_gram_gso_fast_float(any: G, int: scale_digits): list {
   def n = _lll_rows(G)
   def data = _lll_data(G)
   mut mu_rows = list(n)
   mut ptr: norms = f64buf_new(n)
   mut i = 0
   while(i < n){
      def row_i = data[i]
      mut ptr: mu_i = f64buf_new(n)
      mut j = 0
      while(j < i){
         mut f64: s = _lll_float_scaled(row_i[j], scale_digits)
         mut h = 0
         while(h < j){
            s -= f64buf_load(mu_i, h) * f64buf_load(mu_rows[j], h) * f64buf_load(norms, h)
            h += 1
         }
         def f64: den = f64buf_load(norms, j)
         f64buf_store(mu_i, j, den == 0.0 ? 0.0 : s / den)
         j += 1
      }
      mut f64: norm = _lll_float_scaled(row_i[i], scale_digits)
      j = 0
      while(j < i){
         def f64: mij = f64buf_load(mu_i, j)
         norm -= mij * mij * f64buf_load(norms, j)
         j += 1
      }
      if(abs(norm) < 0.000000000001){ norm = 0.0 }
      f64buf_store(norms, i, norm)
      mu_rows = mu_rows.append(mu_i)
      i += 1
   }
   [mu_rows, norms, n]
}

fn _lll_gram_fast_mu(any: gso, int: i, int: j): f64 {
   if(i < 0 || i >= int(gso[2]) || j < 0){ return 0.0 }
   f64buf_load(gso[0][i], j)
}

fn _lll_gram_fast_norm(any: gso, int: i): f64 {
   if(i < 0 || i >= int(gso[2])){ return 0.0 }
   f64buf_load(gso[1], i)
}

fn _lll_gram_fast_lovasz_holds(any: gso, int: k, any: delta): bool {
   if(k <= 0 || k >= int(gso[2])){ return true }
   def f64: muk = _lll_gram_fast_mu(gso, k, k - 1)
   _lll_gram_fast_norm(gso, k) >= (float(delta) - muk * muk) * _lll_gram_fast_norm(gso, k - 1)
}

fn lll_gram_gso_report(any: gram): dict {
   "Return GSO coefficients and squared norms computed directly from a Gram matrix."
   def G = _lll_gram_square(gram)
   def exact_env = env("NY_LATTICE_GRAM_EXACT")
   if(_lll_scale_digits(G) > 0 && ((is_str(exact_env) && (exact_env == "1" || exact_env == "true" || exact_env == "yes")) || _lll_rows(G) <= 8)){ return _lll_gram_gso_report_exact(G) }
   _lll_gram_gso_report_float(G)
}

fn lll_basis_gram_gso_parity_report(any: basis, any: operations=nil, any: tolerance=0.001): dict {
   "Compare basis-derived GSO against direct Gram-matrix GSO, optionally after row operations."
   def t0 = ticks()
   mut B = _lll_as_matrix(basis)
   mut G = _lll_basis_gram(B)
   def ops = operations == nil ? [] : operations
   mut applied = []
   mut i = 0
   while(i < ops.len){
      def op = ops[i]
      def pair = _lll_apply_gso_parity_op(B, G, op)
      B = pair[0]
      G = pair[1]
      applied = applied.append(op)
      i += 1
   }
   def reconstructed = _lll_basis_gram(B)
   def gram_matches_basis = _lll_same_matrix(G, reconstructed)
   def basis_gso = gso_report(B)
   def gram_gso = lll_gram_gso_report(G)
   def diff = _lll_gso_parity_diff(basis_gso, gram_gso)
   def tol = float(tolerance)
   def ok = gram_matches_basis && float(diff.get("max_mu_relative_diff", 1.0)) <= tol && float(diff.get("max_norm_relative_diff", 1.0)) <= tol
   {
      "method": "basis-gram-gso-parity",
      "rows": _lll_rows(B),
      "cols": _lll_cols(B),
      "operations": applied,
      "operation_count": applied.len,
      "tolerance": tol,
      "ok": ok,
      "gram_matches_basis": gram_matches_basis,
      "basis_gso": basis_gso,
      "gram_gso": gram_gso,
      "basis": B,
      "gram": G,
      "reconstructed_gram": reconstructed,
      "max_mu_abs_diff": diff.get("max_mu_abs_diff", 0.0),
      "max_mu_relative_diff": diff.get("max_mu_relative_diff", 0.0),
      "max_norm_relative_diff": diff.get("max_norm_relative_diff", 0.0),
      "worst_mu": diff.get("worst_mu", nil),
      "worst_norm": diff.get("worst_norm", nil),
      "elapsed_ms": _lll_elapsed_ms(t0),
   }
}

fn lll_gso_parity_report(any: basis, any: operations=nil, any: tolerance=0.001): dict {
   "Alias for lll_basis_gram_gso_parity_report."
   lll_basis_gram_gso_parity_report(basis, operations, tolerance)
}

fn _lll_gram_lovasz_holds(any: gso, int: k, any: delta): bool {
   def exact_norms = gso.get("norms_exact", nil)
   def exact_mu = gso.get("mu_exact", nil)
   if(is_list(exact_norms) && is_list(exact_mu) && k > 0 && k < exact_norms.len){
      def muk = _lll_rat_to_float(exact_mu[k][k - 1])
      def lhs = _lll_rat_to_float(exact_norms[k])
      def rhs = (float(delta) - muk * muk) * _lll_rat_to_float(exact_norms[k - 1])
      return lhs >= rhs
   }
   def norms = gso.get("norms_sq", [])
   if(k <= 0 || k >= norms.len){ return true }
   def mu = gso.get("mu")
   def muk = float(_lll_get(mu, k, k - 1))
   float(norms[k]) >= (float(delta) - muk * muk) * float(norms[k - 1])
}

fn _lll_gram_exact_mu_or(any: mu_exact, any: gso, int: k, int: j): any {
   if(is_list(mu_exact) && k >= 0 && k < mu_exact.len){
      def row = mu_exact[k]
      if(is_list(row) && j >= 0 && j < row.len){ return row[j] }
   }
   _lll_get(gso.get("mu"), k, j)
}

fn _lll_gram_quality_from_gso(any: G, any: gso, any: delta, any: eta): dict {
   def n = _lll_rows(G)
   def mu = gso.get("mu")
   mut violations = []
   mut max_mu = 0.0
   mut size_reduced = true
   mut lovasz = true
   mut i = 1
   while(i < n){
      mut j = 0
      while(j < i){
         def a = abs(float(_lll_get(mu, i, j)))
         if(a > max_mu){ max_mu = a }
         if(a > float(eta)){
            size_reduced = false
            violations = violations.append({"kind": "size", "i": i, "j": j, "mu": a})
         }
         j += 1
      }
      if(!_lll_gram_lovasz_holds(gso, i, delta)){
         lovasz = false
         violations = violations.append({"kind": "lovasz", "i": i})
      }
      i += 1
   }
   _lll_set_fields(dict(10), [
         ["method", "gram-lll-quality"], ["reduced", size_reduced && lovasz], ["size_reduced", size_reduced],
         ["lovasz", lovasz], ["max_mu", max_mu], ["gso", gso], ["profile", gso.get("profile", [])],
         ["violations", violations],
   ])
}

fn lll_gram_quality_report(any: gram, any: delta=0.75, any: eta=0.51): dict {
   "Return LLL quality checks computed directly from a Gram matrix."
   def G = _lll_gram_square(gram)
   _lll_gram_quality_from_gso(G, lll_gram_gso_report(G), delta, eta)
}

fn lll_gram_is_reduced(any: gram, any: delta=0.75, any: eta=0.51): bool {
   "Return true when a Gram matrix satisfies LLL quality checks."
   lll_gram_quality_report(gram, delta, eta).get("reduced", false)
}

fn _lll_gram_verify_transform(any: original, any: transform, any: reduced): bool {
   def prod = matrix_mul(matrix_mul(transform, original), matrix_transpose(transform))
   _lll_same_matrix(prod, reduced)
}

fn _lll_round_div_to_z(any: num, any: den): bigint {
   def dz = Z(den)
   if(dz == Z(0)){ return Z(0) }
   _lll_rat_round_to_z(_lll_rat(num, dz))
}

fn _lll_gram_first_column_reduce_state(any: gram): list {
   mut work = _lll_gram_square(gram)
   def n = _lll_rows(work)
   mut transform = _lll_identity(n)
   mut ops = 0
   mut k = 1
   while(k < n){
      def q = _lll_round_div_to_z(_lll_get(work, k, 0), _lll_get(work, 0, 0))
      if(q != Z(0)){
         work = _lll_gram_row_sub_scaled(work, k, 0, q)
         transform = _lll_transform_row_sub_scaled(transform, k, 0, q)
         ops += 1
      }
      k += 1
   }
   [work, transform, ops]
}

fn _lll_gram_first_column_max_mu(any: gram): f64 {
   def G = _lll_gram_square(gram)
   def n = _lll_rows(G)
   def den = _lll_get(G, 0, 0)
   if(den == Z(0)){ return 0.0 }
   mut out = 0.0
   mut k = 1
   while(k < n){
      def a = abs(_lll_rat_to_float(_lll_rat(_lll_get(G, k, 0), den)))
      if(a > out){ out = a }
      k += 1
   }
   out
}

fn lll_gram_first_column_reduce_report(any: gram, any: delta=0.75, any: eta=0.51): dict {
   "Exact first-column Gram size-reduction prepass for high-dynamic cleanup paths."
   def t0 = ticks()
   def G = _lll_gram_square(gram)
   def before = lll_gram_quality_report(G, delta, eta)
   def state = _lll_gram_first_column_reduce_state(G)
   def reduced = state[0]
   def transform = state[1]
   def after = lll_gram_quality_report(reduced, delta, eta)
   _lll_set_fields(dict(18), [
         ["method", "gram-first-column-size-reduce"], ["input_kind", "gram"], ["rows", _lll_rows(G)],
         ["cols", _lll_cols(G)], ["before", before], ["after", after], ["ops", state[2]],
         ["before_violations", before.get("violations", []).len], ["after_violations", after.get("violations", []).len],
         ["improved_violation_count", after.get("violations", []).len < before.get("violations", []).len],
         ["before_max_mu", _lll_gram_first_column_max_mu(G)], ["after_max_mu", _lll_gram_first_column_max_mu(reduced)],
         ["transform", transform], ["transform_verified", _lll_gram_verify_transform(G, transform, reduced)],
         ["elapsed_ms", _lll_elapsed_ms(t0)], ["gram", reduced],
   ])
}

fn _lll_env_positive_int(str: name, int: fallback): int {
   def raw = env(name)
   if(is_str(raw) && raw.len > 0){
      def parsed = atoi(raw)
      if(parsed > 0){ return parsed }
   }
   fallback
}

fn _lll_gram_exact_step_cap(any: gram): int {
   _lll_env_positive_int("NY_LATTICE_GRAM_EXACT_MAX_STEPS", 32)
}

fn _lll_gram_reduce_state(any: gram, any: delta, any: eta, bool: exact=false, int: max_steps=0): list {
   mut work = _lll_gram_square(gram)
   def n = _lll_rows(work)
   mut transform = _lll_identity(n)
   mut k = 1
   mut steps = 0
   mut ok = true
   def scale_digits = _lll_scale_digits(work)
   def digit_budget = max(64, min(2048, (scale_digits + 1) * 8))
   mut total_budget = max(64, n * n * digit_budget)
   if(max_steps > 0 && max_steps < total_budget){ total_budget = max_steps }
   while(k < n && steps < total_budget){
      mut gso = exact ? lll_gram_gso_report(work) : _lll_gram_gso_fast_float(work, scale_digits)
      mut j = k - 1
      while(j >= 0 && steps < total_budget){
         def mu_exact = exact ? gso.get("mu_exact", nil) : nil
         def mu_kj = exact ? _lll_gram_exact_mu_or(mu_exact, gso, k, j) : _lll_gram_fast_mu(gso, k, j)
         if(exact ? _lll_rat_abs_gt_float(mu_kj, eta) : (abs(float(mu_kj)) > float(eta))){
            def q = exact ? _lll_rat_round_to_z(mu_kj) : _lll_round_to_z(mu_kj)
            if(q != Z(0)){
               work = _lll_gram_row_sub_scaled(work, k, j, q)
               transform = _lll_transform_row_sub_scaled(transform, k, j, q)
               steps += 1
               gso = exact ? lll_gram_gso_report(work) : _lll_gram_gso_fast_float(work, scale_digits)
            }
         }
         j -= 1
      }
      if(steps >= total_budget){
         ok = false
      } elif(exact ? _lll_gram_lovasz_holds(gso, k, delta) : _lll_gram_fast_lovasz_holds(gso, k, delta)){
         k += 1
      } else {
         work = _lll_gram_swap(work, k, k - 1)
         transform = matrix_swap_rows(transform, k, k - 1)
         k = max(k - 1, 1)
         steps += 1
      }
   }
   if(k < n && steps >= total_budget){ ok = false }
   [work, transform, steps, ok, total_budget, exact ? "exact-rational" : "float-scaled"]
}

fn _lll_gram_reduce_report_with_cap(any: gram, any: delta=0.75, str: method="ny", any: eta=0.51, int: requested_exact_step_cap=0): dict {
   "Reduce an integer Gram matrix by exact row/column LLL operations and report transform verification."
   def t0 = ticks()
   def G = _lll_gram_square(gram)
   mut before = _lll_gram_quality_from_gso(G, _lll_gram_gso_report_float(G), delta, eta)
   mut state = _lll_gram_reduce_state(G, delta, eta, false, requested_exact_step_cap)
   mut reduced = state[0]
   mut transform = state[1]
   mut after = _lll_gram_quality_from_gso(reduced, _lll_gram_gso_report_float(reduced), delta, eta)
   mut fallback = ""
   mut exact_step_cap = 0
   def exact_env = env("NY_LATTICE_GRAM_EXACT")
   def allow_exact = is_str(exact_env) && (exact_env == "1" || exact_env == "true" || exact_env == "yes")
   if(!after.get("reduced", false) && (_lll_rows(G) <= 8 || allow_exact) && _lll_scale_digits(G) > 0){
      exact_step_cap = requested_exact_step_cap > 0 ? requested_exact_step_cap : _lll_gram_exact_step_cap(G)
      state = _lll_gram_reduce_state(G, delta, eta, true, exact_step_cap)
      reduced = state[0]
      transform = state[1]
      after = lll_gram_quality_report(reduced, delta, eta)
      fallback = "exact-rational"
   }
   def verified = _lll_gram_verify_transform(G, transform, reduced)
   mut out = dict(20)
   out = out.set("method", method)
   out = out.set("selected_method", "ny")
   out = out.set("input_kind", "gram")
   out = out.set("rows", _lll_rows(G))
   out = out.set("cols", _lll_cols(G))
   out = out.set("before", before)
   out = out.set("after", after)
   out = out.set("gso_before", before.get("gso", dict(0)))
   out = out.set("gso_after", after.get("gso", dict(0)))
   out = out.set("profile_before", before.get("profile", []))
   out = out.set("profile_after", after.get("profile", []))
   out = out.set("transform", transform)
   out = out.set("transform_verified", verified)
   out = out.set("state_complete", state[3])
   out = out.set("reduction_complete", state[3] && after.get("reduced", false))
   out = out.set("reduction_kernel", state.len > 5 ? state[5] : "")
   if(fallback != ""){ out = out.set("fallback", fallback) }
   if(state[3] && !after.get("reduced", false)){ out = out.set("incomplete_reason", "quality-check-failed") }
   if(!state[3]){ out = out.set("incomplete_reason", "step-budget-exhausted") }
   out = out.set("steps", state[2])
   if(state.len > 4){ out = out.set("step_budget", state[4]) }
   if(exact_step_cap > 0){ out = out.set("exact_step_cap", exact_step_cap) }
   out = out.set("elapsed_ms", _lll_elapsed_ms(t0))
   out = out.set("gram", reduced)
   out
}

fn lll_gram_reduce_report(any: gram, any: delta=0.75, str: method="ny", any: eta=0.51): dict {
   "Reduce a Gram matrix and return transform, quality, timing, and completion diagnostics."
   _lll_gram_reduce_report_with_cap(gram, delta, method, eta, 0)
}

fn lll_gram_reduce_bounded_report(any: gram, int: exact_step_cap=8, any: delta=0.75, str: method="ny", any: eta=0.51): dict {
   "Reduce a Gram matrix with an explicit exact-step cap; reports incomplete instead of running pathological cleanups."
   _lll_gram_reduce_report_with_cap(gram, delta, method, eta, exact_step_cap)
}

fn lll_gram_report(any: gram, any: delta=0.75, str: method="auto", any: eta=0.51): dict {
   "Report-first Gram-LLL API. `auto` resolves to Ny's exact Gram reducer."
   lll_gram_reduce_report(gram, delta, method, eta)
}

fn lll_gram(any: gram, any: delta=0.75, str: method="ny", any: eta=0.51): any {
   "Return only the reduced Gram matrix."
   lll_gram_reduce_report(gram, delta, method, eta).get("gram")
}

fn lll_reduce_bounded(any: basis, int: step_cap=0, any: delta=0.75, str: method="bounded-fast", any: eta=0.51): any {
   "Run bounded LLL and return only the reduced basis, avoiding report/profile overhead."
   def B = _lll_as_matrix(basis)
   def rows = _lll_rows(B)
   def cols = _lll_cols(B)
   def budget = step_cap > 0 ? step_cap : max(64, rows * cols * 8)
   def row_budget = min(_lll_i_max(4, rows + cols), 128)
   def int_no_transform = method == "bounded-int-no-transform"
   def int_transform = method == "bounded-int-transform"
   if(!int_no_transform && !int_transform && _lll_scale_digits(B) != 0){ return B }
   def track_transform = method != "bounded-fast-no-transform" && !int_no_transform
   mut state = int_no_transform ? _lll_reduce_state_fast_native_no_transform_with_budget(B, delta, eta, budget, row_budget) : (int_transform ? _lll_reduce_state_fast_native_transform_with_budget(B, delta, eta, budget, row_budget) : _lll_reduce_state_fast_list_with_budget(B, delta, eta, budget, row_budget, track_transform))
   if(int_no_transform && !state[3] && int(state[2]) < budget){
      state = _lll_reduce_state_fast_list_with_budget(B, delta, eta, budget, row_budget, false)
   }
   if(int_transform && !state[3] && int(state[2]) < budget){
      state = _lll_reduce_state_fast_list_with_budget(B, delta, eta, budget, row_budget, true)
   }
   state[0]
}

fn lll_quality_report(any: basis, any: delta=0.75, any: eta=0.51): dict {
   "Return standard LLL quality checks: size reduction, Lovasz, profile, and violations."
   def B = _lll_as_matrix(basis)
   def fast = _lll_quality_report_fast_native(B, delta, eta)
   if(fast != nil){ return fast }
   def n = _lll_rows(B)
   def gso = gso_profile(B)
   def b_star = gso.get("b_star")
   def mu = gso.get("mu")
   mut profile = gso.get("norms_sq")
   mut violations = []
   mut max_mu = 0.0
   mut size_reduced = true
   mut lovasz = true
   mut i = 0
   while(i < n){
      mut j = 0
      while(j < i){
         def mu_ij = _lll_get(mu, i, j)
         def a = float(abs(mu_ij))
         if(a > max_mu){ max_mu = a }
         if(a > float(eta)){
            size_reduced = false
            violations = violations.append({
                  "kind": "size", "i": i, "j": j, "mu": mu_ij, "eta": eta,
            })
         }
         j += 1
      }
      if(i > 0){
         def mu_prev = _lll_get(mu, i, i - 1)
         def lhs = profile[i]
         def rhs = (delta - mu_prev * mu_prev) * profile[i - 1]
         if(lhs < rhs){
            lovasz = false
            violations = violations.append({
                  "kind": "lovasz", "i": i, "lhs": lhs, "rhs": rhs,
                  "mu": mu_prev, "delta": delta,
            })
         }
      }
      i += 1
   }
   {
      "rows": n, "cols": _lll_cols(B), "delta": delta, "eta": eta,
      "reduced": size_reduced && lovasz, "is_reduced": size_reduced && lovasz,
      "ok": size_reduced && lovasz, "size_reduced": size_reduced,
      "lovasz": lovasz, "max_mu": max_mu, "profile": profile,
      "violations": violations, "gso": gso,
   }
}

fn lll_is_reduced(any: basis, any: delta=0.75, any: eta=0.51): bool {
   "Return true when a basis satisfies the same LLL checks exposed by lll_quality_report."
   lll_quality_report(basis, delta, eta).get("reduced", false)
}

fn lll_reduce_report(any: basis, any: delta=0.75, str: method="ny", any: eta=0.51): dict {
   "Reduce a basis and return strategy choice, GSO/profile data, transform, timings, and the reduced basis."
   def t0 = ticks()
   def B = _lll_as_matrix(basis)
   def rows0 = _lll_rows(B)
   def cols0 = _lll_cols(B)
   def full_audit_env = env("NY_LLL_FULL_AUDIT_REPORT")
   def full_audit = is_str(full_audit_env) && (full_audit_env == "1" || full_audit_env == "true" || full_audit_env == "yes")
   if(!full_audit && (method == "ny" || method == "auto" || method == "pure" || method == "lll") && rows0 >= 24 && rows0 <= 160 && cols0 <= 256 && _lll_scale_digits(B) == 0){
      def state = _lll_reduce_state_report_fast(B, delta, eta, true)
      def reduced = state[0]
      def transform = state[1]
      def reduction_complete = state.len > 3 ? state[3] : true
      def after_quality = lll_quality_report(reduced, delta, eta)
      def fast_transform_verified = transform != nil ? _lll_transform_verified_fast(transform, B, reduced) : nil
      def transform_verified = transform != nil ? (fast_transform_verified == nil ? _lll_same_matrix(_lll_apply_transform(transform, B), reduced) : bool(fast_transform_verified)) : false
      def elapsed = _lll_elapsed_ms(t0)
      mut out = {
         "method": method, "selected_method": "ny", "delta": delta, "eta": eta,
         "backend": _lll_backend_stub(), "before": _lll_skipped("set NY_LLL_FULL_AUDIT_REPORT=1 for full pre-reduction quality payload"), "after": after_quality,
         "gso_before": _lll_skipped("fast verified LLL report"), "gso_after": after_quality.get("gso", _lll_skipped("fast verified LLL report")),
         "profile_before": [], "profile_after": after_quality.get("profile", _lll_first_profile(reduced)),
         "transform": transform, "transform_tracked": transform != nil,
         "transform_verified": transform_verified,
         "verification_skipped": false, "quality_verified": true,
         "timing": {"reduce_and_verify_ms": elapsed},
         "core": state.len > 4 ? state[4] : "list-gso-int-transform",
         "reduction_complete": reduction_complete,
         "is_reduced": after_quality.get("reduced", false),
         "ok": reduction_complete && after_quality.get("reduced", false) && transform_verified,
         "steps": state[2], "elapsed_ms": elapsed, "basis": reduced
      }
      if(state.len > 5){ out["precision_digits"] = state[5] }
      if(state.len > 6){ out["deep_insertions"] = state[6] }
      if(state.len > 7){ out["insertion_distance"] = state[7] }
      return out
   }
   if(method == "fast" || method == "fast-no-transform"){
      def state = _lll_reduce_state_report_fast(B, delta, eta, method != "fast-no-transform")
      def reduced = state[0]
      def transform = state[1]
      def reduction_complete = state.len > 3 ? state[3] : true
      def after_quality = lll_quality_report(reduced, delta, eta)
      def elapsed = _lll_elapsed_ms(t0)
      mut fast = {
         "method": method, "selected_method": "ny", "delta": delta, "eta": eta,
         "backend": _lll_backend_stub(), "before": dict(0), "after": after_quality,
         "gso_before": _lll_skipped("fast LLL report"), "gso_after": _lll_skipped("fast LLL report"),
         "profile_before": [], "profile_after": _lll_first_profile(reduced),
         "transform": transform, "transform_tracked": transform != nil,
         "transform_verified": false, "verification_skipped": true, "quality_verified": true,
         "timing": {"reduce_ms": elapsed},
         "core": state.len > 4 ? state[4] : "single-violation-gso-transform",
         "reduction_complete": reduction_complete,
         "is_reduced": after_quality.get("reduced", false),
         "ok": reduction_complete && after_quality.get("reduced", false),
         "steps": state[2], "elapsed_ms": elapsed, "basis": reduced
      }
      if(state.len > 5){ fast["precision_digits"] = state[5] }
      if(state.len > 6){ fast["deep_insertions"] = state[6] }
      if(state.len > 7){ fast["insertion_distance"] = state[7] }
      return fast
   }
   mut t_prev = t0
   def before = lll_quality_report(B, delta, eta)
   def before_ms = _lll_elapsed_ms(t_prev)
   mut gso_before = dict(0)
   mut gso_before_ms = 0.0
   def before_gso = before.get("gso", nil)
   if(before_gso != nil){
      gso_before = before_gso
   } else {
      t_prev = ticks()
      gso_before = gso_report(B)
      gso_before_ms = _lll_elapsed_ms(t_prev)
   }
   t_prev = ticks()
   def backend = lll_backend_report(B)
   def backend_ms = _lll_elapsed_ms(t_prev)
   t_prev = ticks()
   mut transform = nil
   mut steps = 0
   def state = _lll_reduce_state_final(B, delta, eta)
   def reduce_ms = _lll_elapsed_ms(t_prev)
   t_prev = ticks()
   def reduced = state[0]
   transform = state[1]
   def recovered_transform = transform == nil ? _lll_recover_transform(B, reduced) : nil
   def recover_ms = _lll_elapsed_ms(t_prev)
   t_prev = ticks()
   transform = recovered_transform != nil ? recovered_transform : transform
   steps = state[2]
   def reduction_complete = state.len > 3 ? state[3] : true
   def after = lll_quality_report(reduced, delta, eta)
   def after_ms = _lll_elapsed_ms(t_prev)
   mut gso_after = dict(0)
   mut gso_after_ms = 0.0
   def after_gso = after.get("gso", nil)
   if(after_gso != nil){
      gso_after = after_gso
   } else {
      t_prev = ticks()
      gso_after = gso_report(reduced)
      gso_after_ms = _lll_elapsed_ms(t_prev)
   }
   t_prev = ticks()
   def fast_transform_verified = transform != nil ? _lll_transform_verified_fast(transform, B, reduced) : nil
   def transform_verified = transform != nil ? (fast_transform_verified == nil ? _lll_same_matrix(_lll_apply_transform(transform, B), reduced) : bool(fast_transform_verified)) : false
   def verify_ms = _lll_elapsed_ms(t_prev)
   mut out = {
      "method": method, "selected_method": "ny", "delta": delta, "eta": eta,
      "backend": backend, "before": before, "after": after,
      "gso_before": gso_before, "gso_after": gso_after,
      "profile_before": gso_before.get("profile"), "profile_after": gso_after.get("profile"),
      "transform": transform, "transform_tracked": transform != nil,
      "transform_verified": transform_verified,
      "timing": {
         "before_quality_ms": before_ms,
         "gso_before_ms": gso_before_ms,
         "backend_ms": backend_ms,
         "reduce_ms": reduce_ms,
         "recover_transform_ms": recover_ms,
         "after_quality_ms": after_ms,
         "gso_after_ms": gso_after_ms,
         "verify_transform_ms": verify_ms
      },
      "core": state.len > 4 ? state[4] : "single-violation-gso-transform",
      "reduction_complete": reduction_complete,
      "is_reduced": after.get("reduced", false),
      "ok": reduction_complete && after.get("reduced", false),
      "steps": steps,
      "elapsed_ms": _lll_elapsed_ms(t0),
      "basis": reduced
   }
   if(state.len > 5){ out["precision_digits"] = state[5] }
   out
}

fn lll_reduce_bounded_report(any: basis, int: step_cap=0, any: delta=0.75, str: method="bounded-fast", any: eta=0.51): dict {
   "Run the list-GSO LLL kernel under an explicit step budget and report incomplete status instead of falling back to slower cleanup paths."
   def t0 = ticks()
   def B = _lll_as_matrix(basis)
   def rows = _lll_rows(B)
   def cols = _lll_cols(B)
   if(method == "bounded-int-no-transform-compact"){
      def budget = step_cap > 0 ? step_cap : max(64, rows * cols * 8)
      def row_budget = min(_lll_i_max(4, rows + cols), 128)
      mut state = _lll_reduce_state_fast_native_no_transform_with_budget(B, delta, eta, budget, row_budget)
      if(!state[3] && int(state[2]) < budget){
         state = _lll_reduce_state_fast_list_with_budget(B, delta, eta, budget, row_budget, false)
      }
      def complete = state.len > 3 ? state[3] : true
      mut out = {
         "method": method, "selected_method": "ny", "delta": delta, "eta": eta,
         "rows": rows, "cols": cols, "backend": _lll_backend_stub(),
         "before": dict(0), "after": dict(0),
         "gso_before": _lll_skipped("bounded compact report"), "gso_after": _lll_skipped("bounded compact report"),
         "profile_before": [], "profile_after": [],
         "first_norm_before": -1, "first_norm_after": -1,
         "best_norm_before": -1, "best_norm_after": -1,
         "transform": nil, "transform_tracked": false,
         "transform_verified": false, "verification_skipped": true, "quality_verified": false,
         "timing": {"reduce_ms": _lll_elapsed_ms(t0)},
         "core": state.len > 4 ? state[4] : "compact-int-no-transform",
         "reduction_complete": complete,
         "steps": state[2], "step_budget": state.len > 5 ? state[5] : budget,
         "row_budget": row_budget, "elapsed_ms": _lll_elapsed_ms(t0), "basis": state[0]
      }
      if(!complete){ out["incomplete_reason"] = "step-budget-exhausted" }
      if(state.len > 6){ out["deep_insertions"] = state[6] }
      if(state.len > 7){ out["insertion_distance"] = state[7] }
      return out
   }
   def before_first = _lll_first_profile(B)
   def before_first_norm = before_first.len > 0 ? before_first.get(0) : Z(0)
   def before_best_norm = _lll_best_row_norm_sq(B)
   if(_lll_scale_digits(B) != 0){
      return {
         "method": method,
         "selected_method": "ny",
         "delta": delta,
         "eta": eta,
         "rows": rows,
         "cols": cols,
         "backend": _lll_backend_stub(),
         "before": dict(0),
         "after": dict(0),
         "gso_before": _lll_skipped("bounded fast report"),
         "gso_after": _lll_skipped("bounded fast report"),
         "profile_before": before_first,
         "profile_after": before_first,
         "first_norm_before": before_first_norm,
         "first_norm_after": before_first_norm,
         "best_norm_before": before_best_norm,
         "best_norm_after": before_best_norm,
         "transform": matrix_identity(rows),
         "transform_tracked": true,
         "transform_verified": false,
         "verification_skipped": true,
         "quality_verified": false,
         "reduction_complete": false,
         "incomplete_reason": "non-integral-basis-unsupported",
         "steps": 0,
         "step_budget": step_cap,
         "elapsed_ms": _lll_elapsed_ms(t0),
         "basis": B
      }
   }
   def budget = step_cap > 0 ? step_cap : max(64, rows * cols * 8)
   def row_budget = min(_lll_i_max(4, rows + cols), 128)
   def int_no_transform = method == "bounded-int-no-transform"
   def int_transform = method == "bounded-int-transform"
   def track_transform = method != "bounded-fast-no-transform" && !int_no_transform
   mut state = int_no_transform ? _lll_reduce_state_fast_native_no_transform_with_budget(B, delta, eta, budget, row_budget) : (int_transform ? _lll_reduce_state_fast_native_transform_with_budget(B, delta, eta, budget, row_budget) : _lll_reduce_state_fast_list_with_budget(B, delta, eta, budget, row_budget, track_transform))
   if(int_no_transform && !state[3] && int(state[2]) < budget){
      state = _lll_reduce_state_fast_list_with_budget(B, delta, eta, budget, row_budget, false)
   }
   if(int_transform && !state[3] && int(state[2]) < budget){
      state = _lll_reduce_state_fast_list_with_budget(B, delta, eta, budget, row_budget, true)
   }
   def reduced = state[0]
   def transform = state[1]
   def complete = state.len > 3 ? state[3] : true
   def elapsed = _lll_elapsed_ms(t0)
   def after_first = _lll_first_profile(reduced)
   def after_first_norm = after_first.len > 0 ? after_first.get(0) : Z(0)
   def after_best_norm = _lll_best_row_norm_sq(reduced)
   mut out = {
      "method": method, "selected_method": "ny", "delta": delta, "eta": eta,
      "rows": rows, "cols": cols, "backend": _lll_backend_stub(),
      "before": dict(0), "after": dict(0),
      "gso_before": _lll_skipped("bounded fast report"), "gso_after": _lll_skipped("bounded fast report"),
      "profile_before": before_first, "profile_after": after_first,
      "first_norm_before": before_first_norm, "first_norm_after": after_first_norm,
      "best_norm_before": before_best_norm, "best_norm_after": after_best_norm,
      "transform": transform, "transform_tracked": transform != nil,
      "transform_verified": false, "verification_skipped": true, "quality_verified": false,
      "timing": {"reduce_ms": elapsed},
      "core": state.len > 4 ? state[4] : "list-gso-transform",
      "reduction_complete": complete,
      "steps": state[2], "step_budget": state.len > 5 ? state[5] : budget,
      "row_budget": row_budget, "elapsed_ms": elapsed, "basis": reduced
   }
   if(!complete){ out["incomplete_reason"] = "step-budget-exhausted" }
   if(state.len > 6){ out["deep_insertions"] = state[6] }
   if(state.len > 7){ out["insertion_distance"] = state[7] }
   out
}

fn lll_report(any: basis, any: delta=0.75, str: method="auto", any: eta=0.51): dict {
   "Report-first public LLL API. `auto` resolves to LLL."
   lll_reduce_report(basis, delta, method, eta)
}

fn lll(any: basis, any: delta=0.75, str: method="ny", any: eta=0.51): any {
   "Perform LLL lattice reduction on a matrix basis.
   - `method=\"ny\"` (default): use LLL.
   - `method=\"auto\"`: resolve to LLL.
   - `method=\"lll\"`: alias for the same implementation."
   if(method == "fast" || method == "fast-no-transform"){
      return lll_reduce_report(basis, delta, method, eta).get("basis")
   }
   if(method == "auto" || method == "pure" || method == "ny" || method == "lll"){ return _lll_pure_reduce(basis, delta, eta) }
   _lll_pure_reduce(basis, delta, eta)
}

fn matrix_get_row(any: m, int: i): list {
   "Internal: get row i from matrix m by reading from the matrix data array."
   def data = _lll_data(m)
   if(i < 0 || i >= data.len){ return [] }
   data[i]
}

fn matrix_set_row(any: m, int: i, any: row): any {
   "Internal: set row i of matrix m to row and return the updated matrix representation."
   def rows = _lll_rows(m)
   def cols = _lll_cols(m)
   def data = _lll_data(m)
   if(i < 0 || i >= rows){ return m }
   mut new_data = list(0)
   mut r = 0
   while(r < rows){
      new_data = new_data.append(data[r])
      r += 1
   }
   mut rr = row
   if(!is_list(rr)){ rr = [] }
   if(rr.len != cols){
      mut fixed = []
      mut j = 0
      while(j < cols){
         fixed = fixed.append((j < rr.len) ? rr[j] : Z(0))
         j += 1
      }
      rr = fixed
   }
   new_data[i] = rr
   [rows, cols, new_data]
}

fn matrix_swap_rows(any: m, int: i, int: j): any {
   "Internal: swap rows i and j in matrix m and return the updated matrix."
   def row_i, row_j = matrix_get_row(m, i), matrix_get_row(m, j)
   mut res = matrix_set_row(m, i, row_j)
   res = matrix_set_row(res, j, row_i)
   res
}

fn dot_product(list: v1, list: v2): any {
   "Internal: compute the dot product of two vectors v1 and v2 using big integer arithmetic."
   mut sum = 0
   mut i = 0
   while(i < v1.len){
      sum = sum + v1[i] * v2[i]
      i += 1
   }
   sum
}

fn vector_sub(list: v1, list: v2): list {
   "Internal: subtract vector v2 from v1 element-wise and return the result."
   mut res = list(0)
   mut i = 0
   while(i < v1.len){
      res = res.append(v1[i] - v2[i])
      i += 1
   }
   res
}

fn vector_scale(list: v, any: s): list {
   "Internal: scale vector v by scalar s element-wise and return the result."
   mut res = list(0)
   mut i = 0
   while(i < v.len){
      res = res.append(v[i] * s)
      i += 1
   }
   res
}

fn abs(any: x): any {
   "Internal: compute the absolute value of x."
   (x < 0) ? (-x) : x
}

fn round(any: x): int {
   "Internal: round x to the nearest integer."
   (x > 0) ? to_int(x + 0.5) : to_int(x - 0.5)
}

if(comptime{ return __main() }){
   def q = lll_quality_report([[1, 0], [0, 1]])
   assert(
      q.get("ok", false) && q.get("is_reduced", false) && q.get("reduced", false),
      "LLL quality report marks identity reduced",
   )
   def r = lll_reduce_report([[1, 1], [1, 0]])
   assert(
      r.get("reduction_complete", false) && r.get("ok", false) && r.get("is_reduced", false),
      "LLL reduce report completes and carries reduced quality",
   )
   print("✓ crypto lattice.lll self-tests passed")
}
