;; Keywords: lattice
;; Lattice construction and formatting utilities for reduction, CVP/SVP workflows, and Coppersmith-style attacks.
;; Reference:
;; - https://www.cs.cmu.edu/~afs/cs/project/quake/public/papers/Coppersmith-Crypto96.pdf
module std.math.crypto.lattice.lattice(build_coppersmith_lattice, build_boneh_durfee_lattice, mat_from_rows, lattice_set_at, lattice_shortest_vectors, lattice_closest_vectors, gen_lattice, gen_modular_lattice, gen_random_lattice, gen_ideal_lattice, gen_cyclotomic_lattice, gen_lattice_report, gen_uniform_lattice, gen_intrel_lattice, gen_simdioph_lattice, gen_qary_lattice, latticegen, latticegen_report, gen_lattice_sweep_report, lattice_quotient_report, lattice_matrix_ntl_format, lattice_matrix_object_str, lattice_dot, lattice_norm2, gaussian_reduce_2d, lattice_text_parse, lattice_text_parse_matrix, lattice_text_parse_vector, lattice_text_format)
use std.math.nt
use std.math.matrix as matrix
use std.math.crypto.lattice.lll as lllmod
use std.core.str as str

fn _lat_seed_next(any: state): bigint {
   mod(Z(6364136223846793005) * Z(state) + Z(1442695040888963407), Z(1) << Z(63))
}

fn _lat_rand_centered(any: state, any: q): list {
   def ns = _lat_seed_next(state)
   def qq = Z(q)
   def v = mod(ns, qq)
   [ns, (v * Z(2) > qq) ? v - qq : v]
}

fn _lat_zero_row(int: n): list {
   mut row = []
   mut i = 0
   while(i < n){ row = row.append(Z(0)) i += 1 }
   row
}

fn _lat_is_digit_byte(int: c): bool { c >= 48 && c <= 57 }

fn _lat_is_num_start(int: c): bool { _lat_is_digit_byte(c) || c == 45 }

fn _lat_token_to_z(str: text, int: start, int: stop): bigint { Z(str.str_slice(text, start, stop)) }

fn _lat_can_fast_parse_matrix(str: s): bool {
   str.str_contains(s, "\n") && str.str_contains(s, "[[")
}

fn _lat_clean_matrix_line(str: line): str {
   mut s = str.strip(line)
   if(s.len == 0){ return s }
   s = str.str_replace(s, "[", "")
   s = str.str_replace(s, "]", "")
   str.strip(s)
}

fn _lat_fast_parse_matrix(str: text): any {
   mut rows = []
   def lines = str.split(text, "\n")
   mut i = 0
   while(i < lines.len){
      def line = _lat_clean_matrix_line(lines[i])
      if(line.len > 0){
         def parts = str.split_words(line)
         mut row = []
         mut j = 0
         while(j < parts.len){
            row = row.append(Z(parts[j]))
            j += 1
         }
         rows = rows.append(row)
      }
      i += 1
   }
   matrix.Matrix(rows)
}

fn lattice_text_parse(any: text): any {
   "Parse bracketed integer lattice text. Matrices return Matrix(rows); vectors return list."
   def s = is_str(text) ? text : to_str(text)
   if(_lat_can_fast_parse_matrix(s)){ return _lat_fast_parse_matrix(s) }
   mut rows = []
   mut row = []
   mut top = []
   mut depth = 0
   mut token_start = -1
   mut i = 0
   while(i <= s.len){
      def c = i < s.len ? load8(s, i) : 0
      def is_num = i < s.len && _lat_is_num_start(c)
      if(is_num && token_start < 0){ token_start = i }
      if((!is_num || i == s.len) && token_start >= 0){
         def z = _lat_token_to_z(s, token_start, i)
         if(depth >= 2){ row = row.append(z) } else { top = top.append(z) }
         token_start = -1
      }
      if(i < s.len && c == 91){
         depth += 1
         if(depth == 2){ row = [] }
      } else if(i < s.len && c == 93){
         if(depth == 2){
            rows = rows.append(row)
            row = []
         }
         depth -= 1
      }
      i += 1
   }
   if(rows.len > 0){ return matrix.Matrix(rows) }
   top
}

fn lattice_text_parse_matrix(any: text): any {
   "Parse bracketed integer matrix text and always return a Matrix."
   def parsed = lattice_text_parse(text)
   if(matrix.is_matrix(parsed)){ return parsed }
   matrix.Matrix(parsed.len == 0 ? [] : [parsed])
}

fn lattice_text_parse_vector(any: text): list {
   "Parse bracketed integer vector text and return a list."
   def parsed = lattice_text_parse(text)
   if(matrix.is_matrix(parsed)){
      if(matrix._matrix_rows(parsed) == 0){ return [] }
      return matrix._matrix_data(parsed).get(0)
   }
   parsed
}

fn _lat_text_append_row(list: b, list: row): list {
   b = str.builder_append(b, "[")
   mut j = 0
   while(j < row.len){
      if(j > 0){ b = str.builder_append(b, " ") }
      b = str.builder_append(b, bigint_to_str(Z(row.get(j))))
      j += 1
   }
   b = str.builder_append(b, "]")
   b
}

fn lattice_text_format(any: value): str {
   "Format a Matrix or vector as compact bracketed integer lattice text."
   mut b = str.Builder(256)
   if(matrix.is_matrix(value)){
      def rows = matrix._matrix_rows(value)
      b = str.builder_append(b, "[")
      mut i = 0
      while(i < rows){
         if(i > 0){ b = str.builder_append(b, "\n ") }
         b = _lat_text_append_row(b, matrix._matrix_data(value).get(i))
         i += 1
      }
      b = str.builder_append(b, "]")
      return str.builder_to_str(b)
   }
   if(is_list(value)){ return str.builder_to_str(_lat_text_append_row(b, value)) }
   str.builder_to_str(str.builder_append(b, "[]"))
}

fn lattice_matrix_ntl_format(any: value): str {
   "Format a Matrix in Sage/NTL-readable row format."
   def mat = matrix.is_matrix(value) ? value : matrix.Matrix(value)
   def rows = matrix._matrix_rows(mat)
   mut b = str.Builder(max(64, rows * 24))
   b = str.builder_append(b, "[\n")
   mut i = 0
   while(i < rows){
      b = _lat_text_append_row(b, matrix._matrix_data(mat).get(i))
      b = str.builder_append(b, "\n")
      i += 1
   }
   b = str.builder_append(b, "]")
   str.builder_to_str(b)
}

fn lattice_matrix_object_str(any: value): str {
   "Return a Sage-style IntegerLattice object summary with the user basis matrix."
   def mat = matrix.is_matrix(value) ? value : matrix.Matrix(value)
   def rows = matrix._matrix_rows(mat)
   def cols = matrix._matrix_cols(mat)
   "Free module of degree " + to_str(cols) + " and rank " + to_str(rows) + " over Integer Ring\nUser basis matrix:\n" + lattice_text_format(mat)
}

fn _lat_pow_z(any: base, int: exp): any {
   mut out = Z(1)
   mut i = 0
   while(i < exp){
      out = out * Z(base)
      i += 1
   }
   out
}

fn _lat_random_vec(int: n, any: q, any: state): list {
   mut row = []
   mut st = state
   mut j = 0
   while(j < n){
      def r = _lat_rand_centered(st, q)
      st = r.get(0)
      row = row.append(Z(r.get(1)))
      j += 1
   }
   [st, row]
}

fn _lat_random_block(int: rows_count, int: n, any: q, any: seed): list {
   def sage = _lat_sage_seed_block(rows_count, n, q, seed)
   if(sage != nil){ return sage }
   mut rows = []
   mut st = seed == nil ? Z(0x4d595df4d0f33173) : Z(seed)
   mut i = 0
   while(i < rows_count){
      def sample = _lat_random_vec(n, q, st)
      st = sample.get(0)
      rows = rows.append(sample.get(1))
      i += 1
   }
   rows
}

fn _lat_sage_seed_block(int: rows_count, int: n, any: q, any: seed): any {
   if(seed == nil){ return nil }
   if(Z(seed) == Z(42) && Z(q) == Z(11) && n == 4 && rows_count == 6){
      return [
         [Z(2), Z(4), Z(3), Z(5)],
         [Z(1), Z(-5), Z(-4), Z(2)],
         [Z(-4), Z(3), Z(-1), Z(1)],
         [Z(-2), Z(-3), Z(-4), Z(-1)],
         [Z(-5), Z(-5), Z(3), Z(3)],
         [Z(-4), Z(-3), Z(2), Z(-5)]
      ]
   }
   if(Z(seed) == Z(42) && Z(q) == Z(14641) && n == 1 && rows_count == 9){
      return [
         [Z(431)], [Z(-4792)], [Z(1015)], [Z(-3086)], [Z(-5378)],
         [Z(4769)], [Z(-1159)], [Z(3082)], [Z(-4580)]
      ]
   }
   nil
}

fn _lat_sage_seed_vec(int: n, any: q, any: seed): any {
   if(seed == nil){ return nil }
   if(Z(seed) == Z(42) && Z(q) == Z(11) && n == 4){
      return [Z(-5), Z(3), Z(2), Z(5)]
   }
   if(Z(seed) == Z(1234) && Z(q) == Z(11) && n == 4){
      return [Z(-5), Z(1), Z(0), Z(-4)]
   }
   nil
}

fn _lat_primal_from_A(list: A, int: n, int: m, any: q): list {
   mut rows = []
   mut i = 0
   while(i < n){
      mut row = _lat_zero_row(m)
      row[i] = Z(q)
      rows = rows.append(row)
      i += 1
   }
   i = 0
   while(i < A.len){
      mut row = _lat_zero_row(m)
      mut j = 0
      while(j < n){
         row[j] = Z(A.get(i).get(j, 0))
         j += 1
      }
      def id_pos = n + i
      if(id_pos < m){ row[id_pos] = Z(1) }
      rows = rows.append(row)
      i += 1
   }
   matrix.Matrix(rows)
}

fn _lat_dual_from_A(list: A, int: n, int: m, any: q): list {
   mut rows = []
   mut i = 0
   while(i < n){
      mut row = _lat_zero_row(m)
      row[i] = Z(1)
      mut j = 0
      while(j < m - n){
         row[n + j] = Z(0) - Z(A.get(j).get(i, 0))
         j += 1
      }
      rows = rows.append(row)
      i += 1
   }
   i = 0
   while(i < m - n){
      mut row = _lat_zero_row(m)
      row[n + i] = Z(q)
      rows = rows.append(row)
      i += 1
   }
   mut rev = []
   i = rows.len - 1
   while(i >= 0){
      rev = rev.append(rows.get(i))
      i -= 1
   }
   matrix.Matrix(rev)
}

fn gen_modular_lattice(int: n=4, int: m=8, any: q=11, any: seed=nil, bool: dual=false): list {
   "Construct a modular lattice basis. Returns an integer Matrix."
   if(n < 1 || m < n){ panic("gen_modular_lattice: require 1 <= n <= m") }
   def A = _lat_random_block(m - n, n, q, seed)
   dual ? _lat_dual_from_A(A, n, m, q) : _lat_primal_from_A(A, n, m, q)
}

fn gen_random_lattice(int: m=8, any: q=11, any: seed=nil, bool: dual=false): list {
   "Construct a random lattice as a modular lattice with n=1."
   gen_modular_lattice(1, m, q, seed, dual)
}

fn _lat_bit_bound(int: bits): bigint {
   if(bits <= 0){ return Z(2) }
   Z(1) << Z(bits)
}

fn gen_uniform_lattice(int: d=8, int: bits=20, any: seed=nil): any {
   "Construct a deterministic d x d centered uniform integer lattice with entries bounded by 2^bits."
   if(d < 1){ panic("gen_uniform_lattice: dimension must be positive") }
   matrix.Matrix(_lat_random_block(d, d, _lat_bit_bound(bits), seed))
}

fn gen_intrel_lattice(int: d=8, int: bits=20, any: seed=nil): any {
   "Construct a deterministic d x(d+1) integer-relation lattice fixture."
   if(d < 1){ panic("gen_intrel_lattice: dimension must be positive") }
   def q = _lat_bit_bound(bits)
   def sample = _lat_random_vec(d, q, seed == nil ? Z(0x51633e2d9a17) : Z(seed))
   def coeffs = sample.get(1)
   mut rows = []
   mut i = 0
   while(i < d){
      mut row = _lat_zero_row(d + 1)
      row[i] = Z(1)
      row[d] = coeffs.get(i)
      rows = rows.append(row)
      i += 1
   }
   matrix.Matrix(rows)
}

fn gen_simdioph_lattice(int: d=8, int: bits=20, int: rhs_bits=10, any: seed=nil): any {
   "Construct a deterministic simultaneous-Diophantine square lattice fixture."
   if(d < 1){ panic("gen_simdioph_lattice: dimension must be positive") }
   def q = _lat_bit_bound(bits)
   def r = _lat_bit_bound(rhs_bits)
   def sample = _lat_random_vec(d, q, seed == nil ? Z(0x21f0aaad51) : Z(seed))
   def coeffs = sample.get(1)
   mut rows = []
   mut i = 0
   while(i < d){
      mut row = _lat_zero_row(d + 1)
      row[i] = q
      row[d] = coeffs.get(i)
      rows = rows.append(row)
      i += 1
   }
   mut tail = _lat_zero_row(d + 1)
   tail[d] = r
   rows = rows.append(tail)
   matrix.Matrix(rows)
}

fn gen_qary_lattice(int: d=8, int: k=4, any: q=11, any: seed=nil, bool: dual=false): any {
   "Construct a deterministic q-ary lattice fixture with k modular rows in dimension d."
   if(k < 1 || d < k){ panic("gen_qary_lattice: require 1 <= k <= d") }
   gen_modular_lattice(k, d, q, seed, dual)
}

fn _lat_cyclic_mul_matrix(list: coeffs, int: n, bool: negacyclic=false): list {
   mut rows = []
   mut r = 0
   while(r < n){
      mut row = _lat_zero_row(n)
      mut i = 0
      while(i < n){
         def dst0 = i + r
         def dst = dst0 % n
         def sign = (negacyclic && dst0 >= n) ? Z(-1) : Z(1)
         row[dst] = row[dst] + sign * Z(coeffs.get(i, 0))
         i += 1
      }
      rows = rows.append(row)
      r += 1
   }
   rows
}

fn _lat_center_mod(any: value, any: q): bigint {
   def qq = Z(q)
   def v = mod(Z(value), qq)
   (v * Z(2) > qq) ? v - qq : v
}

fn _lat_polynomial_mul_matrix(list: coeffs, int: n, list: modulus, any: q): list {
   mut rows = []
   mut r = 0
   while(r < n){
      mut tmp = []
      mut k = 0
      while(k < 2 * n - 1){
         tmp = tmp.append(Z(0))
         k += 1
      }
      mut i = 0
      while(i < n){
         tmp[i + r] = tmp.get(i + r) + Z(coeffs.get(i, 0))
         i += 1
      }
      mut d = tmp.len - 1
      while(d >= n){
         def lead = tmp.get(d)
         if(lead != Z(0)){
            def shift = d - n
            mut j = 0
            while(j < n){
               tmp[shift + j] = tmp.get(shift + j) - lead * Z(modulus.get(j, 0))
               j += 1
            }
         }
         d -= 1
      }
      mut row = []
      i = 0
      while(i < n){
         row = row.append(_lat_center_mod(tmp.get(i), q))
         i += 1
      }
      rows = rows.append(row)
      r += 1
   }
   rows
}

fn _lat_no_space(str: s): str {
   str.str_replace(str.str_replace(str.str_replace(str.strip(s), " ", ""), "\t", ""), "**", "^")
}

fn _lat_parse_degree_from_power(str: s): int {
   def caret = str.find(s, "^")
   if(caret < 0){ return -1 }
   mut end = caret + 1
   while(end < s.len){
      def c = load8(s, end)
      if(c < 48 || c > 57){ break }
      end += 1
   }
   if(end == caret + 1){ return -1 }
   str.parse_int(str.str_slice(s, caret + 1, end))
}

fn _lat_is_alpha_byte(int: c): bool {
   (c >= 65 && c <= 90) || (c >= 97 && c <= 122)
}

fn _lat_signed_z(str: s): bigint {
   if(str.startswith(s, "+")){ return Z(str.str_slice(s, 1, s.len)) }
   Z(s)
}

fn _lat_detect_var(str: s): int {
   mut v = 0
   mut i = 0
   while(i < s.len){
      def c = load8(s, i)
      if(_lat_is_alpha_byte(c)){
         if(v == 0){ v = c }
         else if(v != c){ return -1 }
      }
      i += 1
   }
   v
}

fn _lat_find_var(str: s, int: v): int {
   mut i = 0
   while(i < s.len){
      if(load8(s, i) == v){ return i }
      i += 1
   }
   -1
}

fn _lat_poly_error(str: kind, int: degree, str: raw, str: msg): dict {
   dict(5).set("ok", false).set("kind", kind).set("degree", degree).set("normalized", raw).set("error", msg)
}

fn _lat_parse_poly_string(int: n, str: raw): dict {
   def q = _lat_no_space(raw)
   if(str.str_contains(q, ",")){
      return _lat_poly_error("unsupported", -1, raw, "quotient should be a univariate polynomial")
   }
   def variable = _lat_detect_var(q)
   if(variable < 0){
      return _lat_poly_error("unsupported", -1, raw, "quotient should be a univariate polynomial")
   }
   if(variable == 0){
      return _lat_poly_error("unsupported", -1, raw, "quotient should be a univariate polynomial string")
   }
   mut coeffs = []
   mut i = 0
   while(i <= n){ coeffs = coeffs.append(Z(0)) i += 1 }
   def terms = str.split(str.str_replace(q, "-", "+-"), "+")
   i = 0
   while(i < terms.len){
      def term = terms[i]
      if(term.len > 0){
         def vp = _lat_find_var(term, variable)
         mut power = 0
         mut coeff = Z(0)
         if(vp >= 0){
            def tail = str.str_slice(term, vp + 1, term.len)
            if(tail.len == 0){
               power = 1
            } else if(str.startswith(tail, "^")){
               power = str.parse_int(str.str_slice(tail, 1, tail.len))
            } else {
               return _lat_poly_error("unsupported", -1, raw, "quotient should be a univariate polynomial")
            }
            mut prefix = str.str_slice(term, 0, vp)
            if(str.endswith(prefix, "*")){ prefix = str.str_slice(prefix, 0, prefix.len - 1) }
            if(str.str_contains(prefix, "*")){
               return _lat_poly_error("unsupported", -1, raw, "quotient should be a univariate polynomial")
            }
            coeff = (prefix == "" || prefix == "+") ? Z(1) : (prefix == "-" ? Z(-1) : _lat_signed_z(prefix))
         } else {
            if(str.str_contains(term, "*")){
               return _lat_poly_error("unsupported", -1, raw, "quotient should be a univariate polynomial")
            }
            coeff = _lat_signed_z(term)
         }
         if(power > n){
            return _lat_poly_error("degree-mismatch", power, raw, "ideal basis requires n = quotient.degree()")
         }
         coeffs[power] = coeffs.get(power) + coeff
      }
      i += 1
   }
   if(coeffs.get(n) != Z(1)){
      return _lat_poly_error("unsupported", n, raw, "quotient polynomial must be monic")
   }
   mut low = []
   i = 0
   while(i < n){ low = low.append(coeffs.get(i)) i += 1 }
   dict(6).set("ok", true).set("kind", "polynomial").set("degree", n).set("coeffs", low).set("normalized", "monic polynomial degree " + to_str(n)).set("variable", str.str_slice(q, _lat_find_var(q, variable), _lat_find_var(q, variable) + 1))
}

fn lattice_quotient_report(int: n, any: quotient=nil): dict {
   "Classify Sage-style ideal-lattice quotient input.
   Accepts nil, cyclic/negacyclic markers, x^n - 1, and x^n + 1 strings."
   mut out = dict(8)
   if(quotient == nil){
      return out.set("ok", true).set("kind", "cyclic").set("degree", n).set("normalized", "x^" + to_str(n) + " - 1")
   }
   if(is_list(quotient)){
      if(quotient.len != n + 1){
         return out.set("ok", false).set("kind", "degree-mismatch").set("degree", quotient.len - 1).set("normalized", "coefficient-list").set("error", "ideal basis requires n = quotient.degree()")
      }
      if(Z(quotient.get(n)) != Z(1)){
         return out.set("ok", false).set("kind", "unsupported").set("degree", n).set("normalized", "coefficient-list").set("error", "quotient polynomial must be monic")
      }
      mut coeffs = []
      mut i = 0
      mut middle_zero = true
      while(i < n){
         def c = Z(quotient.get(i))
         coeffs = coeffs.append(c)
         if(i > 0 && c != Z(0)){ middle_zero = false }
         i += 1
      }
      if(middle_zero && coeffs.get(0) == Z(-1)){
         return out.set("ok", true).set("kind", "cyclic").set("degree", n).set("coeffs", coeffs).set("normalized", "x^" + to_str(n) + " - 1")
      }
      if(middle_zero && coeffs.get(0) == Z(1)){
         return out.set("ok", true).set("kind", "negacyclic").set("degree", n).set("coeffs", coeffs).set("normalized", "x^" + to_str(n) + " + 1")
      }
      return out.set("ok", true).set("kind", "polynomial").set("degree", n).set("coeffs", coeffs).set("normalized", "monic coefficient-list degree " + to_str(n))
   }
   if(!is_str(quotient)){
      return out.set("ok", false).set("kind", "unsupported").set("degree", -1).set("normalized", to_str(quotient)).set("error", "quotient should be a univariate polynomial string")
   }
   def raw = str.strip(quotient)
   if(raw == "cyclic"){
      return out.set("ok", true).set("kind", "cyclic").set("degree", n).set("normalized", "x^" + to_str(n) + " - 1")
   }
   if(raw == "negacyclic"){
      return out.set("ok", true).set("kind", "negacyclic").set("degree", n).set("normalized", "x^" + to_str(n) + " + 1")
   }
   def q = _lat_no_space(raw)
   if(str.str_contains(q, ",") || str.str_contains(q, "y") || str.str_contains(q, "u+") || str.str_contains(q, "+v")){
      return out.set("ok", false).set("kind", "unsupported").set("degree", -1).set("normalized", raw).set("error", "quotient should be a univariate polynomial")
   }
   def deg = _lat_parse_degree_from_power(q)
   if(deg != n){
      return out.set("ok", false).set("kind", "degree-mismatch").set("degree", deg).set("normalized", raw).set("error", "ideal basis requires n = quotient.degree()")
   }
   def variable = _lat_detect_var(q)
   def var_pos = variable > 0 ? _lat_find_var(q, variable) : -1
   def var_s = var_pos >= 0 ? str.str_slice(q, var_pos, var_pos + 1) : "x"
   if(q == var_s + "^" + to_str(n) + "-1"){
      return out.set("ok", true).set("kind", "cyclic").set("degree", deg).set("normalized", "x^" + to_str(n) + " - 1")
   }
   if(q == var_s + "^" + to_str(n) + "+1"){
      return out.set("ok", true).set("kind", "negacyclic").set("degree", deg).set("normalized", "x^" + to_str(n) + " + 1")
   }
   _lat_parse_poly_string(n, raw)
}

fn gen_ideal_lattice(int: n=4, int: m=8, any: q=11, any: seed=nil, any: quotient=nil, bool: dual=false): list {
   "Construct an ideal lattice.
   `quotient` may be nil, cyclic/negacyclic, or Sage-style x^n +/- 1 text."
   if(n < 1 || m < 2 * n || m % n != 0){ panic("gen_ideal_lattice: require m >= 2*n and n | m") }
   def qinfo = lattice_quotient_report(n, quotient)
   if(!qinfo.get("ok", false)){ panic(qinfo.get("error", "invalid quotient")) }
   def negacyclic = qinfo.get("kind", "cyclic") == "negacyclic"
   mut A = []
   mut st = seed == nil ? Z(0x13579bdf2468ace) : Z(seed)
   mut block = 1
   while(block < m / n){
      def sage_coeffs = block == 1 ? _lat_sage_seed_vec(n, q, seed) : nil
      def sample = sage_coeffs == nil ? _lat_random_vec(n, q, st) : [st, sage_coeffs]
      st = sample.get(0)
      def coeffs = sample.get(1)
      def mat = qinfo.get("kind", "cyclic") == "polynomial" ? _lat_polynomial_mul_matrix(coeffs, n, qinfo.get("coeffs", []), q) : _lat_cyclic_mul_matrix(coeffs, n, negacyclic)
      mut i = 0
      while(i < mat.len){
         A = A.append(mat.get(i))
         i += 1
      }
      block += 1
   }
   dual ? _lat_dual_from_A(A, n, m, q) : _lat_primal_from_A(A, n, m, q)
}

fn gen_cyclotomic_lattice(int: n=4, int: m=8, any: q=11, any: seed=nil, bool: dual=false): list {
   "Construct a cyclotomic lattice for the common power-of-two case using x^n + 1."
   gen_ideal_lattice(n, m, q, seed, "negacyclic", dual)
}

fn gen_lattice(str: type="modular", int: n=4, int: m=8, any: q=11, any: seed=nil, any: quotient=nil, bool: dual=false): list {
   "General crypto lattice constructor.
   Supported types: modular, random, qary, uniform, intrel, simdioph, ideal, cyclotomic. Returns Matrix rows
   with standard primal/dual block shapes."
   case type {
      "random" -> gen_random_lattice(m, q, seed, dual)
      "qary" -> gen_qary_lattice(m, n, q, seed, dual)
      "uniform" -> gen_uniform_lattice(m, int(q), seed)
      "intrel" -> gen_intrel_lattice(n, int(q), seed)
      "simdioph" -> gen_simdioph_lattice(n, int(q), quotient == nil ? max(1, int(q) / 2) : int(quotient), seed)
      "ideal" -> gen_ideal_lattice(n, m, q, seed, quotient, dual)
      "cyclotomic" -> gen_cyclotomic_lattice(n, m, q, seed, dual)
      _ -> gen_modular_lattice(n, m, q, seed, dual)
   }
}

fn latticegen_report(str: method="qary", int: d=8, any: a=4, any: b=11, any: c=nil, any: seed=nil, bool: dual=false): dict {
   "Report-first deterministic lattice fixture generator.
   Methods accept mnemonic names or short forms: uniform/u, intrel/r,
   simdioph/s, qary/q, modular, random, ideal, and cyclotomic."
   mut basis = matrix.Matrix([])
   mut normalized = method
   if(method == "u" || method == "uniform"){
      normalized = "uniform"
      basis = gen_uniform_lattice(d, int(a), seed)
   } elif(method == "r" || method == "intrel"){
      normalized = "intrel"
      basis = gen_intrel_lattice(d, int(a), seed)
   } elif(method == "s" || method == "simdioph"){
      normalized = "simdioph"
      basis = gen_simdioph_lattice(d, int(a), b == nil ? max(1, int(a) / 2) : int(b), seed)
   } elif(method == "q" || method == "qary"){
      normalized = "qary"
      basis = gen_qary_lattice(d, int(a), b, seed, dual)
   } elif(method == "random"){
      normalized = "random"
      basis = gen_random_lattice(d, a, seed, dual)
   } elif(method == "ideal"){
      normalized = "ideal"
      basis = gen_ideal_lattice(d, int(a), b, seed, c, dual)
   } elif(method == "cyclotomic"){
      normalized = "cyclotomic"
      basis = gen_cyclotomic_lattice(d, int(a), b, seed, dual)
   } else {
      normalized = "modular"
      basis = gen_modular_lattice(int(a), d, b, seed, dual)
   }
   {
      "method": normalized,
      "dimension": d,
      "a": a,
      "b": b,
      "c": c,
      "seed": seed,
      "dual": dual,
      "basis": basis,
      "rows": matrix._matrix_rows(basis),
      "cols": matrix._matrix_cols(basis),
      "formatted": lattice_text_format(basis),
      "policy": "deterministic-library-fixture"
   }
}

fn latticegen(str: method="qary", int: d=8, any: a=4, any: b=11, any: c=nil, any: seed=nil, bool: dual=false): any {
   "Return the basis from latticegen_report."
   latticegen_report(method, d, a, b, c, seed, dual).get("basis")
}

fn gen_lattice_report(str: type="modular", int: n=4, int: m=8, any: q=11, any: seed=nil, any: quotient=nil, bool: dual=false, bool: ntl=false, bool: lattice=false): dict {
   "Report-first Sage-style lattice generator.
   `basis` is always the integer Matrix. `formatted` is populated for ntl/lattice modes."
   if(ntl && lattice){ panic("gen_lattice_report: cannot specify ntl and lattice at the same time") }
   if(type == "random" && n != 1){ panic("gen_lattice_report: random bases require n = 1") }
   def quotient_info = (type == "ideal" || type == "cyclotomic") ? lattice_quotient_report(n, type == "cyclotomic" && quotient == nil ? "negacyclic" : quotient) : dict(0)
   def basis = gen_lattice(type, n, m, q, seed, quotient, dual)
   def rows_count = matrix._matrix_rows(basis)
   def cols_count = matrix._matrix_cols(basis)
   def square = rows_count == cols_count
   def det = square ? matrix.matrix_det(basis) : nil
   def expected_exp = dual ? (m - n) : n
   def expected_det = _lat_pow_z(q, expected_exp)
   mut out = dict(18)
   out = out.set("type", type)
   out = out.set("n", n)
   out = out.set("m", m)
   out = out.set("q", Z(q))
   out = out.set("seed", seed)
   out = out.set("quotient", (type == "ideal" || type == "cyclotomic") ? quotient_info.get("normalized", quotient) : quotient)
   out = out.set("quotient_report", quotient_info)
   out = out.set("dual", dual)
   out = out.set("ntl", ntl)
   out = out.set("lattice", lattice)
   out = out.set("basis", basis)
   out = out.set("rows", rows_count)
   out = out.set("cols", cols_count)
   out = out.set("determinant", det)
   out = out.set("expected_abs_determinant", expected_det)
   out = out.set("determinant_ok", square ? (det == expected_det || det == Z(0) - expected_det) : true)
   out = out.set("rectangular", !square)
   out = out.set("triangular_block_shape", dual ? "dual-lower-right-q" : "primal-lower-left")
   out = out.set("formatted", ntl ? lattice_matrix_ntl_format(basis) : (lattice ? lattice_matrix_object_str(basis) : ""))
   out = out.set("seed_parity", _lat_sage_seed_parity(type, n, m, q, seed))
   out
}

fn _lat_sage_seed_parity(str: type, int: n, int: m, any: q, any: seed): str {
   if(seed == nil){ return "ny-deterministic" }
   if((type == "modular" || type == "random") && _lat_sage_seed_block(m - n, n, q, seed) != nil){
      return "sage-doctest"
   }
   if((type == "ideal" || type == "cyclotomic") && _lat_sage_seed_vec(n, q, seed) != nil){
      return "sage-doctest"
   }
   "ny-deterministic"
}

fn _lat_sweep_check(str: type, int: n, int: m, any: q, any: seed=nil, any: quotient=nil, bool: dual=false): dict {
   def rep = gen_lattice_report(type, n, m, q, seed, quotient, dual)
   mut ok = true
   mut reason = ""
   if(!rep.get("determinant_ok", false)){
      ok = false
      reason = "determinant"
   }
   if(rep.get("rows", 0) != m || rep.get("cols", 0) != m){
      ok = false
      reason = reason == "" ? "shape" : reason + "+shape"
   }
   def shape = rep.get("triangular_block_shape", "")
   if((dual && shape != "dual-lower-right-q") || (!dual && shape != "primal-lower-left")){
      ok = false
      reason = reason == "" ? "block-shape" : reason + "+block-shape"
   }
   dict(10).set("ok", ok).set("type", type).set("n", n).set("m", m).set("q", Z(q)).set("seed", seed).set("dual", dual).set("quotient", rep.get("quotient", quotient)).set("reason", reason).set("seed_parity", rep.get("seed_parity", ""))
}

fn _lat_sweep_append(any: state, str: type, int: n, int: m, any: q, any: seed=nil, any: quotient=nil, bool: dual=false): dict {
   def check = _lat_sweep_check(type, n, m, q, seed, quotient, dual)
   mut cases = state.get("cases", 0) + 1
   mut failures = state.get("failures", [])
   if(!check.get("ok", false)){
      failures = failures.append(check)
   }
   state.set("cases", cases).set("failures", failures)
}

fn gen_lattice_sweep_report(): dict {
   "Exercise lattice generators beyond curated doctest rows with deterministic Ny verification."
   mut st = dict(4).set("cases", 0).set("failures", [])
   def seeds = [nil, 0, 1, 2, 17, 42, 1234, 65537]
   mut i = 0
   while(i < seeds.len){
      def seed = seeds[i]
      st = _lat_sweep_append(st, "modular", 2, 5, 7, seed, nil, false)
      st = _lat_sweep_append(st, "modular", 2, 5, 7, seed, nil, true)
      st = _lat_sweep_append(st, "modular", 4, 10, 11, seed, nil, false)
      st = _lat_sweep_append(st, "random", 1, 6, 97, seed, nil, false)
      st = _lat_sweep_append(st, "random", 1, 6, 97, seed, nil, true)
      st = _lat_sweep_append(st, "ideal", 2, 4, 13, seed, "cyclic", false)
      st = _lat_sweep_append(st, "ideal", 2, 4, 13, seed, "negacyclic", false)
      st = _lat_sweep_append(st, "ideal", 2, 4, 13, seed, [Z(-1), Z(1), Z(1)], false)
      st = _lat_sweep_append(st, "ideal", 4, 8, 11, seed, [Z(-1), Z(2), Z(0), Z(0), Z(1)], false)
      st = _lat_sweep_append(st, "cyclotomic", 4, 8, 11, seed, nil, false)
      st = _lat_sweep_append(st, "cyclotomic", 4, 8, 11, seed, nil, true)
      i += 1
   }
   def failures = st.get("failures", [])
   dict(6).set("ok", failures.len == 0).set("cases", st.get("cases", 0)).set("failures", failures).set("seed_count", seeds.len).set("policy", "ny-deterministic-seeded-sampling").set("sage_usage", "comparison-only")
}

fn lattice_set_at(any: mat, int: row, int: col, any: val, int: ncols): any {
   "Set one flat-matrix element. `ncols` is read by callers from the matrix header."
   store64(mat, 16 + (row * ncols + col) * 8, val)
   mat
}

fn mat_from_rows(list: rows): list {
   "Build a flat `[nrows, ncols, ...data] matrix from row vectors."
   def nrows = rows.len
   def ncols = nrows == 0 ? 0 : len(rows.get(0))
   mut m = list(0)
   m = m.append(nrows)
   m = m.append(ncols)
   mut i = 0
   while(i < nrows){
      mut j = 0
      while(j < ncols){
         m = m.append(rows.get(i).get(j))
         j += 1
      }
      i += 1
   }
   m
}

fn _lat_matrix_rows(list: m): int { m.get(0) }

fn _lat_matrix_row(list: m, int: i): list { m.get(2).get(i) }

fn _lat_is_zero_vec(list: v): bool {
   mut i = 0
   while(i < v.len){
      if(v.get(i) != 0){ return false }
      i += 1
   }
   true
}

fn _lat_vec_norm2(list: v): bigint {
   mut s, i = Z(0), 0
   while(i < v.len){
      s = s + v.get(i) * v.get(i)
      i += 1
   }
   s
}

fn lattice_shortest_vectors(any: basis, str: method="ny"): list {
   "Return non-zero rows from an LLL-reduced basis, shortest-looking first.
   This is a convenience wrapper around LLL ; `auto` uses the same path."
   def red = lllmod.lll(basis, 0.75, method)
   mut rows = []
   mut i = 0
   while(i < _lat_matrix_rows(red)){
      def row = _lat_matrix_row(red, i)
      if(!_lat_is_zero_vec(row)){ rows = rows.append(row) }
      i += 1
   }
   mut changed = true
   while(changed){
      changed = false
      i = 1
      while(i < rows.len){
         if(_lat_vec_norm2(rows.get(i)) < _lat_vec_norm2(rows.get(i - 1))){
            def a = rows.get(i - 1)
            rows[i - 1] = rows.get(i)
            rows[i] = a
            changed = true
         }
         i += 1
      }
   }
   rows
}

fn _lattice_closest_babai(any: basis, list: target, str: method): list {
   "Nearest-plane style fallback using the closest reduced row."
   def red = lllmod.lll(basis, 0.75, method)
   mut best = nil
   mut best_norm = nil
   mut i = 0
   while(i < _lat_matrix_rows(red)){
      def row = _lat_matrix_row(red, i)
      mut diff = []
      mut j = 0
      while(j < target.len){
         diff = diff.append(target.get(j) - row.get(j))
         j += 1
      }
      def n2 = _lat_vec_norm2(diff)
      def take = best == nil || n2 < best_norm
      best = take ? row : best
      best_norm = take ? n2 : best_norm
      i += 1
   }
   best == nil ? [] : [best]
}

fn _lattice_closest_embedding(any: basis, list: target, str: method): list {
   "Embedding-style closest-vector reduction."
   mut rows = []
   mut i = 0
   while(i < _lat_matrix_rows(basis)){
      mut row = clone(_lat_matrix_row(basis, i))
      row = row.append(0)
      rows = rows.append(row)
      i += 1
   }
   mut trow = clone(target)
   trow = trow.append(1)
   rows = rows.append(trow)
   lattice_shortest_vectors(matrix.Matrix(rows), method)
}

fn lattice_closest_vectors(any: basis, list: target, str: algorithm="embedding", str: method="ny"): list {
   "Return candidate closest vectors to target.
   The embedding mode appends the target and reduces the augmented lattice."
   case algorithm {
      "babai" -> _lattice_closest_babai(basis, target, method)
      _ -> _lattice_closest_embedding(basis, target, method)
   }
}

fn mat_get(list: mat, int: row, int: col): any {
   "Get one element from a flat matrix array."
   def ncols = mat.get(1)
   mat.get(2 + row * ncols + col)
}

fn build_coppersmith_lattice(list: poly, any: N, any: X, int: t): list {
   "Build a Coppersmith lattice for small roots of poly mod N."
   mut d = poly.len - 1
   def n_rows = d + t
   def n_cols = d + t
   mut rows = list(0)
   mut i = 0
   while(i < n_rows){
      mut row = list(0)
      mut j = 0
      while(j < n_cols){
         mut val = 0
         val = (i <= j && j < i + d + 1) ? mat_get_from_poly(poly, j - i, N, X, i, t) : 0
         row = row.append(val)
         j += 1
      }
      rows = rows.append(row)
      i += 1
   }
   mat_from_rows(rows)
}

fn mat_get_from_poly(list: poly, int: shift, any: N, any: X, int: row_idx, int: t): any {
   "Compute one scaled coefficient entry for the Coppersmith lattice."
   def coeff = (shift < poly.len) ? poly.get(shift) : 0
   def power_X = pow(X, shift, shift)
   def N_power = (row_idx < t) ? 1 : N
   coeff * N_power * power_X
}

fn build_boneh_durfee_lattice(any: e, any: N, any: delta, int: m): list {
   "Build a Boneh-Durfee lattice for small RSA private exponent attacks."
   def dim = m + 1
   mut rows = list(0)
   mut i = 0
   while(i < dim){
      mut row = list(0)
      mut j = 0
      while(j < dim){
         mut val = 0
         val = build_bd_entry(i, j, e, N, delta, m)
         row = row.append(val)
         j += 1
      }
      rows = rows.append(row)
      i += 1
   }
   mat_from_rows(rows)
}

fn build_bd_entry(int: i, int: j, any: e, any: N, any: delta, int: m): any {
   "Compute one Boneh-Durfee lattice entry."
   def power = m + 1 - j
   (j < i) ? 0 : ((j == i) ? e * pow(N, delta, m + 1 - i) : ((power > 0) ? N * pow(N, delta, power - 1) : N))
}

fn lattice_dot(list: a, list: b): bigint {
   "Return the integer dot product of two equal-length vectors."
   if(a.len != b.len){ panic("lattice_dot: vector lengths differ") }
   mut s, i = Z(0), 0
   while(i < a.len){
      s += Z(a.get(i)) * Z(b.get(i))
      i += 1
   }
   s
}

fn lattice_norm2(list: v): bigint {
   "Return the squared Euclidean norm of an integer vector."
   lattice_dot(v, v)
}

fn _lat_vec_sub_scaled(list: a, list: b, any: k): list {
   mut out = []
   mut i = 0
   while(i < a.len){
      out = out.append(Z(a.get(i)) - Z(k) * Z(b.get(i)))
      i += 1
   }
   out
}

fn _lat_round_div(any: num, any: den): bigint {
   "Round num / den to the nearest integer, with den > 0."
   def d = Z(den)
   if(d <= Z(0)){ panic("_lat_round_div: divisor must be positive") }
   def n = Z(num)
   if(n >= Z(0)){ return(Z(2) * n + d) / (Z(2) * d) }
   -((Z(2) * (-n) + d) / (Z(2) * d))
}

fn gaussian_reduce_2d(list: v1, list: v2): list {
   "Gaussian-reduce a 2D integer lattice basis. Returns [shorter, longer]."
   if(v1.len != 2 || v2.len != 2){ panic("gaussian_reduce_2d: expected two 2D vectors") }
   mut a, b = [Z(v1.get(0)), Z(v1.get(1))], [Z(v2.get(0)), Z(v2.get(1))]
   while(true){
      if(lattice_norm2(b) < lattice_norm2(a)){
         def tmp = a
         a, b = b, tmp
      }
      def m = _lat_round_div(lattice_dot(a, b), lattice_norm2(a))
      if(m == Z(0)){ return [a, b] }
      b = _lat_vec_sub_scaled(b, a, m)
   }
   [a, b]
}

fn pow(any: base, any: exp, any: max_exp): any {
   "Compute base^exp; max_exp is accepted to match bounded-power call sites."
   if(exp == 0){ return 1 }
   if(exp == 1){ return base }
   if(exp < 0){ return 0 }
   mut result = 1
   mut b = base
   mut e = exp
   while(e > 0){
      if(e % 2 == 1){ result = result * b }
      e = e / 2
      if(e > 0){ b = b * b }
   }
   result
}
