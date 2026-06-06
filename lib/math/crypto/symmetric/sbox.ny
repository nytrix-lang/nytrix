;; Keywords: symmetric sbox math crypto
;; Symmetric-crypto routines for S-box construction, metrics, algebraic forms, and tables.
;; References:
;; - std.math.crypto.symmetric
;; - std.math.crypto
module std.math.crypto.symmetric.sbox(sbox_repr, sbox_len, sbox_equal, sbox_not_equal, sbox_get, sbox_items, sbox_input_size, sbox_output_size, sbox_to_bits, sbox_from_bits, sbox_coeff_vector, sbox_from_coeff_vector, sbox_apply_gf2m, sbox_apply, sbox_solutions, sbox_is_permutation, sbox_inverse, sbox_fixed_points, sbox_ddt, sbox_difference_distribution_table, sbox_differential_uniformity, sbox_lat, sbox_linear_approximation_table, sbox_linearity, sbox_maximal_linear_bias_absolute, sbox_maximal_linear_bias_relative, sbox_maximal_difference_probability_absolute, sbox_maximal_difference_probability, sbox_nonlinearity, sbox_derivative, sbox_component_function, sbox_autocorrelation_table, sbox_component_anf, sbox_polynomials, sbox_interpolation_polynomial, sbox_interpolation_polynomial_gf2m, sbox_interpolation_polynomial_gf2m_str, sbox_eval_polynomial_gf2m, sbox_from_polynomial_gf2m, sbox_from_interpolation_polynomial_gf2m, sbox_is_monomial_function, sbox_monomial_str, sbox_component_anf_str, sbox_polynomial_strs, sbox_degree_fit_polynomial_strs, sbox_degree_fit_basis_report, sbox_direct_polynomial_strs, sbox_groebner_polynomial_strs, sbox_groebner_basis_report, sbox_ring_str, sbox_ring, sbox_eval_anf, sbox_cnf, sbox_cnf_clauses, sbox_cnf_satisfied, sbox_boomerang_connectivity_table, sbox_boomerang_uniformity, sbox_linear_structures, sbox_has_linear_structure, sbox_is_linear_structure, sbox_is_apn, sbox_is_balanced, sbox_is_almost_bent, sbox_is_bent, sbox_is_plateaued, sbox_is_involution, sbox_feistel_construction, sbox_misty_construction, sbox_branch_number, sbox_differential_branch_number, sbox_linear_branch_number, sbox_algebraic_degree, sbox_max_degree, sbox_min_degree)
use std.math.bin as bin
use std.math.matrix as matrix

fn _sbox_pow2(int n) bool { n > 0 && (n & (n - 1)) == 0 }

fn _sbox_bits_for_cardinality(int n) int {
   mut bits = 0
   mut p = 1
   while(p < n){
      p = p << 1
      bits += 1
   }
   bits
}

fn _sbox_check(list S) any { if(S.len == 0 || !_sbox_pow2(S.len)){ panic("sbox: lookup table length must be a power of 2") } }

fn _sbox_output_cardinality(list S) int {
   mut max_v = 0
   mut i = 0
   while(i < S.len){
      def v = S[i]
      if(v < 0){ panic("sbox: lookup values must be non-negative") }
      if(v > max_v){ max_v = v }
      i += 1
   }
   1 << _sbox_bits_for_cardinality(max_v + 1)
}

fn _sbox_zero_row(int n) list {
   mut row = list(n)
   __list_set_len(row, n)
   mut i = 0
   while(i < n){
      __store_item_fast(row, i, 0)
      i += 1
   }
   row
}

fn _sbox_parity(int x) int { bin.bit_count(x) & 1 }

fn _sbox_abs_int(int x) int { x < 0 ? 0 - x : x }

fn _sbox_default_width(list S, any n, bool output) int {
   if(n != nil){ return int(n) }
   output ? sbox_output_size(S) : sbox_input_size(S)
}

fn sbox_repr(list S) str {
   "Return Sage SBox repr formatting, e.g. (7, 6, 0, ...)."
   _sbox_check(S)
   mut out = "("
   mut i = 0
   while(i < S.len){
      if(i > 0){ out = out + ", " }
      out = out + to_str(S.get(i))
      i += 1
   }
   out + ")"
}

fn sbox_len(list S) int {
   "Return Sage-compatible len(SBox(...)): the input bit width."
   sbox_input_size(S)
}

fn sbox_equal(list a, list b, bool a_big_endian=true, bool b_big_endian=true) bool {
   "Return Sage-compatible SBox equality over lookup table and endian flag."
   if(a_big_endian != b_big_endian){ return false }
   if(a.len != b.len){ return false }
   mut i = 0
   while(i < a.len){
      if(a.get(i) != b.get(i)){ return false }
      i += 1
   }
   true
}

fn sbox_not_equal(list a, list b, bool a_big_endian=true, bool b_big_endian=true) bool {
   "Return Sage-compatible SBox inequality over lookup table and endian flag."
   !sbox_equal(a, b, a_big_endian, b_big_endian)
}

fn sbox_get(list S, any i) any {
   "Sage-compatible item access for SBox tables."
   _sbox_check(S)
   S.get(i)
}

fn sbox_items(list S) list {
   "Return lookup values in Sage SBox iteration order."
   _sbox_check(S)
   clone(S)
}

fn sbox_input_size(list S) int {
   "Return the input bit width of a power-of-two S-box table."
   _sbox_check(S)
   _sbox_bits_for_cardinality(S.len)
}

fn sbox_output_size(list S) int {
   "Return the minimum output bit width covering all table values."
   _sbox_check(S)
   _sbox_bits_for_cardinality(_sbox_output_cardinality(S))
}

fn sbox_to_bits(list S, int x, any n=nil, bool big_endian=true) list {
   "Return a fixed-width bit list for x using Sage-compatible endian order."
   def width = _sbox_default_width(S, n, true)
   mut out = []
   mut i = 0
   while(i < width){
      out = out.append((x >> i) & 1)
      i += 1
   }
   if(big_endian){
      mut rev = []
      i = out.len - 1
      while(i >= 0){
         rev = rev.append(out.get(i))
         i -= 1
      }
      return rev
   }
   out
}

fn sbox_from_bits(list S, list bits, any n=nil, bool big_endian=true) int {
   "Return the integer represented by a bit list using Sage-compatible endian order."
   def width = _sbox_default_width(S, n, false)
   if(bits.len > width){ panic("sbox_from_bits: bit list is wider than requested width") }
   mut work = []
   mut i = 0
   while(i < bits.len){
      work = work.append(int(bits.get(i)))
      i += 1
   }
   while(work.len < width){ work = work.append(0) }
   mut out = 0
   if(big_endian){
      i = 0
      while(i < width){
         out = (out << 1) | work.get(i)
         i += 1
      }
   } else {
      i = 0
      while(i < width){
         out = out | (work.get(i) << i)
         i += 1
      }
   }
   out
}

fn sbox_coeff_vector(int x, int width) list {
   "Return the GF(2^m) coefficient vector used by Sage finite-field SBox calls."
   if(width < 0){ panic("sbox_coeff_vector: width must be non-negative") }
   if(x < 0){ panic("sbox_coeff_vector: value must be non-negative") }
   mut out = []
   mut i = 0
   while(i < width){
      out = out.append((x >> i) & 1)
      i += 1
   }
   out
}

fn sbox_from_coeff_vector(list coeffs) int {
   "Return an integer from a GF(2^m) coefficient vector in Sage order."
   mut out = 0
   mut i = 0
   while(i < coeffs.len){
      out = out | ((int(coeffs.get(i)) & 1) << i)
      i += 1
   }
   out
}

fn sbox_apply_gf2m(list S, int x, bool big_endian=true) int {
   "Apply S using Sage finite-field element semantics over GF(2^m) coefficient vectors."
   def m = sbox_input_size(S)
   if(sbox_output_size(S) > m){ panic("sbox_apply_gf2m: output does not fit the input extension degree") }
   if(x < 0 || x >= (1 << m)){ panic("sbox_apply_gf2m: element outside GF(2^m) coefficient range") }
   sbox_from_coeff_vector(sbox_apply(S, sbox_coeff_vector(x, m), big_endian))
}

fn sbox_apply(list S, any x, bool big_endian=true) any {
   "Apply an S-box to an integer or to an input-width bit list."
   _sbox_check(S)
   if(is_list(x)){
      if(x.len != sbox_input_size(S)){ panic("sbox_apply: bit input length must match input size") }
      def idx = sbox_from_bits(S, x, sbox_input_size(S), big_endian)
      return sbox_to_bits(S, S.get(idx), sbox_output_size(S), big_endian)
   }
   if(is_str(x) || is_dict(x) || x == nil){ panic("sbox_apply: cannot apply SBox to provided element") }
   def idx = int(x)
   if(is_float(x) && float(idx) != x){ panic("sbox_apply: cannot apply SBox to non-integral numeric input") }
   if(idx >= S.len || idx < (0 - S.len)){ panic("sbox_apply: integer input out of range") }
   S.get(idx)
}

fn sbox_solutions(list S, bool big_endian=true) list {
   "Return Sage-style input/output bit assignments as [input_bits, output_bits]."
   _sbox_check(S)
   mut out = []
   mut x = 0
   while(x < S.len){
      def xb = sbox_to_bits(S, x, sbox_input_size(S), big_endian)
      def yb = sbox_apply(S, xb, big_endian)
      out = out.append([xb, yb])
      x += 1
   }
   out
}

fn sbox_is_permutation(list S) bool {
   "Return true when the S-box is a permutation of 0..len(S)-1."
   _sbox_check(S)
   mut seen = _sbox_zero_row(S.len)
   mut i = 0
   while(i < S.len){
      def v = __load_item_fast(S, i)
      if(v < 0 || v >= S.len || __load_item_fast(seen, v) != 0){ return false }
      __store_item_fast(seen, v, 1)
      i += 1
   }
   true
}

fn sbox_inverse(list S) list {
   "Return the inverse lookup table for a permutation S-box."
   if(!sbox_is_permutation(S)){ panic("sbox_inverse: table is not a permutation") }
   mut inv = _sbox_zero_row(S.len)
   mut i = 0
   while(i < S.len){
      __store_item_fast(inv, __load_item_fast(S, i), i)
      i += 1
   }
   inv
}

fn sbox_fixed_points(list S) list {
   "Return all inputs x where S[x] == x."
   _sbox_check(S)
   mut out = []
   mut i = 0
   while(i < S.len){
      if(S.get(i) == i){ out = out.append(i) }
      i += 1
   }
   out
}

fn sbox_ddt(list S) any {
   "Return the differential distribution table as a Matrix."
   _sbox_check(S)
   def n = S.len
   def out_n = _sbox_output_cardinality(S)
   mut rows = list(n)
   __list_set_len(rows, n)
   mut dx = 0
   while(dx < n){
      mut row = _sbox_zero_row(out_n)
      mut x = 0
      while(x < n){
         def dy = __load_item_fast(S, x) ^^ __load_item_fast(S, x ^^ dx)
         __store_item_fast(row, dy, __load_item_fast(row, dy) + 1)
         x += 1
      }
      __store_item_fast(rows, dx, row)
      dx += 1
   }
   matrix.Matrix(rows)
}

fn sbox_difference_distribution_table(list S) any {
   "Sage-compatible alias for the difference distribution table."
   sbox_ddt(S)
}

fn sbox_differential_uniformity(list S) int {
   "Return max DDT entry excluding the zero input difference."
   _sbox_check(S)
   def n = S.len
   def out_n = _sbox_output_cardinality(S)
   mut best = 0
   mut dx = 1
   while(dx < n){
      mut row = _sbox_zero_row(out_n)
      mut x = 0
      while(x < n){
         def dy = __load_item_fast(S, x) ^^ __load_item_fast(S, x ^^ dx)
         def v = __load_item_fast(row, dy) + 1
         __store_item_fast(row, dy, v)
         if(v > best){ best = v }
         x += 1
      }
      dx += 1
   }
   best
}

fn sbox_differential_uniformity_from_ddt(list S) int {
   "Return max DDT entry excluding the zero input difference from the materialized table."
   def ddt = sbox_ddt(S)
   def out_n = _sbox_output_cardinality(S)
   mut best = 0
   mut dx = 1
   while(dx < S.len){
      mut dy = 0
      while(dy < out_n){
         def v = matrix.mat_get(ddt, dx, dy)
         if(v > best){ best = v }
         dy += 1
      }
      dx += 1
   }
   best
}

fn sbox_maximal_difference_probability_absolute(list S) int {
   "Return Sage-compatible maximum DDT entry excluding [0,0]."
   sbox_differential_uniformity(S)
}

fn sbox_maximal_difference_probability(list S) f64 {
   "Return Sage-compatible maximum differential probability."
   float(sbox_maximal_difference_probability_absolute(S)) / float(1 << sbox_output_size(S))
}

fn _sbox_walsh_table(list S) any {
   "Return unscaled Walsh/Fourier coefficients for internal linear tests."
   _sbox_check(S)
   def in_n = S.len
   def out_n = _sbox_output_cardinality(S)
   mut rows = list(in_n)
   __list_set_len(rows, in_n)
   mut a = 0
   while(a < in_n){
      __store_item_fast(rows, a, _sbox_zero_row(out_n))
      a += 1
   }
   mut b = 0
   while(b < out_n){
      mut vec = list(in_n)
      __list_set_len(vec, in_n)
      mut x = 0
      while(x < in_n){
         __store_item_fast(vec, x, (_sbox_parity(b & S[x]) == 0) ? 1 : -1)
         x += 1
      }
      mut step = 1
      while(step < in_n){
         mut base = 0
         while(base < in_n){
            x = 0
            while(x < step){
               def u = vec[base + x]
               def v = vec[base + x + step]
               __store_item_fast(vec, base + x, u + v)
               __store_item_fast(vec, base + x + step, u - v)
               x += 1
            }
            base += step * 2
         }
         step = step << 1
      }
      a = 0
      while(a < in_n){
         rows[a][b] = vec[a]
         a += 1
      }
      b += 1
   }
   matrix.Matrix(rows)
}

fn sbox_linear_approximation_table(list S, any scale="absolute_bias") any {
   "Return the Sage-compatible LAT scaled as absolute_bias, bias, correlation, or fourier_coefficient."
   def mode = scale == nil ? "absolute_bias" : to_str(scale)
   def walsh = _sbox_walsh_table(S)
   def wdata = matrix._matrix_data(walsh)
   def in_bits = sbox_input_size(S)
   def out_n = _sbox_output_cardinality(S)
   mut rows = list(S.len)
   __list_set_len(rows, S.len)
   mut a = 0
   while(a < S.len){
      def wrow = wdata[a]
      mut row = list(out_n)
      __list_set_len(row, out_n)
      mut b = 0
      while(b < out_n){
         def v = wrow[b]
         if(mode == "absolute_bias"){
            __store_item_fast(row, b, v / 2)
         } elif(mode == "bias"){
            __store_item_fast(row, b, float(v) / float(1 << (in_bits + 1)))
         } elif(mode == "correlation"){
            __store_item_fast(row, b, float(v) / float(1 << in_bits))
         } elif(mode == "fourier_coefficient"){
            __store_item_fast(row, b, v)
         } else {
            panic("sbox_linear_approximation_table: no such scaling for the LAT: " + mode)
         }
         b += 1
      }
      __store_item_fast(rows, a, row)
      a += 1
   }
   matrix.Matrix(rows)
}

fn sbox_lat(list S) any {
   "Return the Sage-default linear approximation table(absolute_bias scale)."
   sbox_linear_approximation_table(S, "absolute_bias")
}

fn sbox_linearity(list S) int {
   "Return Sage-compatible linearity: twice the maximum absolute bias."
   _sbox_check(S)
   def in_n = S.len
   def out_n = _sbox_output_cardinality(S)
   mut best = 0
   mut b = 0
   while(b < out_n){
      mut vec = list(in_n)
      __list_set_len(vec, in_n)
      mut x = 0
      while(x < in_n){
         __store_item_fast(vec, x, (_sbox_parity(b & __load_item_fast(S, x)) == 0) ? 1 : -1)
         x += 1
      }
      mut step = 1
      while(step < in_n){
         mut base = 0
         while(base < in_n){
            x = 0
            while(x < step){
               def u = __load_item_fast(vec, base + x)
               def v = __load_item_fast(vec, base + x + step)
               __store_item_fast(vec, base + x, u + v)
               __store_item_fast(vec, base + x + step, u - v)
               x += 1
            }
            base += step * 2
         }
         step = step << 1
      }
      mut a = 0
      while(a < in_n){
         if(a != 0 || b != 0){
            mut v = __load_item_fast(vec, a)
            if(v < 0){ v = 0 - v }
            if(v > best){ best = v }
         }
         a += 1
      }
      b += 1
   }
   best
}

fn sbox_maximal_linear_bias_absolute(list S) int {
   "Return Sage-compatible maximum absolute linear bias."
   sbox_linearity(S) / 2
}

fn sbox_maximal_linear_bias_relative(list S) f64 {
   "Return Sage-compatible maximum relative linear bias."
   float(sbox_maximal_linear_bias_absolute(S)) / float(S.len)
}

fn sbox_nonlinearity(list S) int {
   "Return the minimum nonlinearity over all non-zero component functions."
   (S.len / 2) - sbox_maximal_linear_bias_absolute(S)
}

fn _sbox_input_mask(list S, any mask, bool big_endian=true) int {
   def in_bits = sbox_input_size(S)
   if(is_list(mask)){
      if(mask.len > in_bits){ panic("sbox input mask: bit list is wider than input size") }
      return sbox_from_coeff_vector(mask)
   }
   int(mask)
}

fn _sbox_input_mask_loose(list S, any mask, bool big_endian=true) int {
   if(is_list(mask)){ return sbox_from_coeff_vector(mask) }
   int(mask)
}

fn sbox_derivative(list S, any u, bool big_endian=true) list {
   "Return the derivative x -> S[x] xor S[x xor u]."
   _sbox_check(S)
   def v = _sbox_input_mask(S, u, big_endian)
   mut out = []
   mut x = 0
   while(x < S.len){
      out = out.append(__load_item_fast(S, x) ^^ __load_item_fast(S, x ^^ v))
      x += 1
   }
   out
}

fn _sbox_component_mask(list S, any mask, bool big_endian=true) int {
   def out_bits = sbox_output_size(S)
   if(is_list(mask)){
      if(mask.len > out_bits){ panic("sbox component mask: bit list is wider than output size") }
      return sbox_from_bits(S, mask, out_bits, big_endian)
   }
   int(mask)
}

fn sbox_component_function(list S, any mask, bool big_endian=true) list {
   "Return the truth table of the component function mask dot S(x)."
   _sbox_check(S)
   def m = _sbox_component_mask(S, mask, big_endian)
   mut out = []
   mut x = 0
   while(x < S.len){
      out = out.append(_sbox_parity(m & __load_item_fast(S, x)))
      x += 1
   }
   out
}

fn _sbox_anf_from_truth_table(list table) list {
   if(table.len == 0 || !_sbox_pow2(table.len)){ panic("sbox ANF: truth table length must be a power of 2") }
   mut coeff = []
   mut i = 0
   while(i < table.len){
      coeff = coeff.append(int(table.get(i)) & 1)
      i += 1
   }
   mut step = 1
   while(step < coeff.len){
      mut mask = 0
      while(mask < coeff.len){
         if((mask & step) != 0){ coeff[mask] = coeff.get(mask) ^^ coeff.get(mask ^^ step) }
         mask += 1
      }
      step = step << 1
   }
   mut out = []
   i = 0
   while(i < coeff.len){
      if(coeff.get(i) != 0){ out = out.append(i) }
      i += 1
   }
   out
}

fn sbox_component_anf(list S, any mask, bool big_endian=true) list {
   "Return ANF monomial masks for the component function mask dot S(x)."
   _sbox_anf_from_truth_table(sbox_component_function(S, mask, big_endian))
}

fn sbox_polynomials(list S) list {
   "Return output-bit ANFs as monomial-mask lists, ordered from low bit to high bit."
   _sbox_check(S)
   def out_bits = sbox_output_size(S)
   mut polys = []
   mut bit = 0
   while(bit < out_bits){
      polys = polys.append(sbox_component_anf(S, 1 << bit))
      bit += 1
   }
   polys
}

fn sbox_interpolation_polynomial(list S) list {
   "Return the vectorial interpolation form as output-bit ANF monomial masks."
   sbox_polynomials(S)
}

fn _sbox_gf_modulus(int m) int {
   if(m == 1){ return 0b11 }
   if(m == 2){ return 0b111 }
   if(m == 3){ return 0b1011 }
   if(m == 4){ return 0b10011 }
   if(m == 5){ return 0b100101 }
   if(m == 6){ return 0b1000011 }
   if(m == 7){ return 0b10000011 }
   if(m == 8){ return 0b100011011 }
   panic("sbox GF interpolation: supported extension degrees are 1..8")
}

fn _sbox_bit_reverse(int x, int width) int {
   mut out = 0
   mut i = 0
   while(i < width){
      out = (out << 1) | ((x >> i) & 1)
      i += 1
   }
   out
}

fn _sbox_gf_mul(int a, int b, int m, int poly) int {
   def mask = (1 << m) - 1
   mut aa = a & mask
   mut bb = b & mask
   mut out = 0
   while(bb != 0){
      if((bb & 1) != 0){ out = out ^^ aa }
      bb = bb >> 1
      aa = aa << 1
      if((aa & (1 << m)) != 0){ aa = aa ^^ poly }
   }
   out & mask
}

fn _sbox_gf_pow(int a, int e, int m, int poly) int {
   mut base = a
   mut exp = e
   mut out = 1
   while(exp > 0){
      if((exp & 1) != 0){ out = _sbox_gf_mul(out, base, m, poly) }
      base = _sbox_gf_mul(base, base, m, poly)
      exp = exp >> 1
   }
   out
}

fn _sbox_gf_inv(int a, int m, int poly) int {
   if(a == 0){ panic("sbox GF interpolation: zero denominator") }
   _sbox_gf_pow(a, (1 << m) - 2, m, poly)
}

fn _sbox_poly_mul_linear_gf(list p, int root, int m, int poly) list {
   mut out = []
   mut i = 0
   while(i <= p.len){
      out = out.append(0)
      i += 1
   }
   i = 0
   while(i < p.len){
      def c = int(p.get(i))
      out[i] = int(out.get(i)) ^^ _sbox_gf_mul(c, root, m, poly)
      out[i + 1] = int(out.get(i + 1)) ^^ c
      i += 1
   }
   out
}

fn sbox_eval_polynomial_gf2m(list coeffs, int x, int m) int {
   "Evaluate low-degree-first coefficients over GF(2^m) using Sage-compatible field constants."
   if(coeffs.len == 0){ return 0 }
   def poly = _sbox_gf_modulus(m)
   mut out = 0
   mut i = coeffs.len - 1
   while(i >= 0){
      out = _sbox_gf_mul(out, x, m, poly) ^^ (int(coeffs.get(i)) & ((1 << m) - 1))
      i -= 1
   }
   out
}

fn sbox_from_polynomial_gf2m(list coeffs, int m) list {
   "Build the Sage SBox(poly) lookup table by evaluating over sorted GF(2^m) elements."
   def q = 1 << m
   mut out = []
   mut x = 0
   while(x < q){
      out = out.append(sbox_eval_polynomial_gf2m(coeffs, x, m))
      x += 1
   }
   out
}

fn sbox_from_interpolation_polynomial_gf2m(list coeffs, int m) list {
   "Build a lookup table from coefficients returned by sbox_interpolation_polynomial_gf2m."
   def q = 1 << m
   mut out = []
   mut i = 0
   while(i < q){
      def x = _sbox_bit_reverse(i, m)
      def y = sbox_eval_polynomial_gf2m(coeffs, x, m)
      out = out.append(_sbox_bit_reverse(y, m))
      i += 1
   }
   out
}

fn sbox_interpolation_polynomial_gf2m(list S) list {
   "Return Sage-compatible univariate GF(2^m) interpolation coefficients, low degree first."
   _sbox_check(S)
   def m = sbox_input_size(S)
   if(sbox_output_size(S) != m){
      panic("sbox GF interpolation: input and output sizes must match")
   }
   def q = 1 << m
   def poly = _sbox_gf_modulus(m)
   mut coeffs = []
   mut i = 0
   while(i < q){
      coeffs = coeffs.append(0)
      i += 1
   }
   i = 0
   while(i < q){
      def xi = _sbox_bit_reverse(i, m)
      def yi = _sbox_bit_reverse(int(S.get(i)), m)
      mut basis = [1]
      mut den = 1
      mut j = 0
      while(j < q){
         if(j != i){
            def xj = _sbox_bit_reverse(j, m)
            basis = _sbox_poly_mul_linear_gf(basis, xj, m, poly)
            den = _sbox_gf_mul(den, xi ^^ xj, m, poly)
         }
         j += 1
      }
      def scale = _sbox_gf_mul(yi, _sbox_gf_inv(den, m, poly), m, poly)
      j = 0
      while(j < basis.len){
         coeffs[j] = int(coeffs.get(j)) ^^ _sbox_gf_mul(int(basis.get(j)), scale, m, poly)
         j += 1
      }
      i += 1
   }
   while(coeffs.len > 1 && int(coeffs.get(coeffs.len - 1)) == 0){
      coeffs = slice(coeffs, 0, coeffs.len - 1)
   }
   coeffs
}

fn _sbox_gf_coeff_str(int c, int m) str {
   if(c == 0){ return "0" }
   mut out = ""
   mut p = m - 1
   while(p >= 0){
      if(((c >> p) & 1) != 0){
         mut part = ""
         if(p == 0){ part = "1" }
         else if(p == 1){ part = "a" }
         else { part = "a^" + to_str(p) }
         out = out.len == 0 ? part : (out + " + " + part)
      }
      p -= 1
   }
   out
}

fn _sbox_gf_poly_term_str(int coeff, int exp, int m, str variable) str {
   def c = _sbox_gf_coeff_str(coeff, m)
   if(exp == 0){ return c }
   def x = exp == 1 ? variable : (variable + "^" + to_str(exp))
   if(coeff == 1){ return x }
   def cc = bin.bit_count(coeff) > 1 ? ("(" + c + ")") : c
   cc + "*" + x
}

fn sbox_interpolation_polynomial_gf2m_str(list S, str variable="x") str {
   "Return a Sage-style GF(2^m) interpolation polynomial string."
   def coeffs = sbox_interpolation_polynomial_gf2m(S)
   def m = sbox_input_size(S)
   mut out = ""
   mut e = coeffs.len - 1
   while(e >= 0){
      def c = int(coeffs.get(e))
      if(c != 0){
         def term = _sbox_gf_poly_term_str(c, e, m, variable)
         out = out.len == 0 ? term : (out + " + " + term)
      }
      e -= 1
   }
   out.len == 0 ? "0" : out
}

fn sbox_is_monomial_function(list S) bool {
   "Return true when the GF(2^m) interpolation polynomial has one non-zero term."
   def coeffs = sbox_interpolation_polynomial_gf2m(S)
   mut count = 0
   mut i = 0
   while(i < coeffs.len){
      if(int(coeffs.get(i)) != 0){ count += 1 }
      i += 1
   }
   count == 1
}

fn sbox_ring_str(list S, str x_prefix="x", str y_prefix="y") str {
   "Return a Sage-style description of the Boolean polynomial ring for S-box equations."
   def m = sbox_input_size(S)
   def n = sbox_output_size(S)
   mut vars = ""
   mut i = 0
   while(i < m){
      vars = vars.len == 0 ? (x_prefix + to_str(i)) : (vars + ", " + x_prefix + to_str(i))
      i += 1
   }
   i = 0
   while(i < n){
      vars = vars.len == 0 ? (y_prefix + to_str(i)) : (vars + ", " + y_prefix + to_str(i))
      i += 1
   }
   "Multivariate Polynomial Ring in " + vars + " over Finite Field of size 2"
}

fn sbox_ring(list S, str x_prefix="x", str y_prefix="y") str {
   "Sage-compatible ring() alias returning the graph polynomial ring description."
   sbox_ring_str(S, x_prefix, y_prefix)
}

fn sbox_monomial_str(int monomial_mask, int input_bits, str variable_prefix="x", bool big_endian=true) str {
   "Format one ANF monomial mask using Sage-style variable names."
   if(monomial_mask == 0){ return "1" }
   mut out = ""
   mut pos = 0
   while(pos < input_bits){
      def bit_pos = big_endian ? (input_bits - 1 - pos) : pos
      if(((monomial_mask >> bit_pos) & 1) != 0){
         def name = variable_prefix + to_str(pos)
         out = out.len == 0 ? name : (out + "*" + name)
      }
      pos += 1
   }
   out
}

fn _sbox_terms_str_desc(list monomials, int input_bits, str variable_prefix, bool big_endian) str {
   mut out = ""
   mut idx = monomials.len - 1
   while(idx >= 0){
      def term = sbox_monomial_str(int(monomials.get(idx)), input_bits, variable_prefix, big_endian)
      out = out.len == 0 ? term : (out + " + " + term)
      idx -= 1
   }
   out.len == 0 ? "0" : out
}

fn sbox_component_anf_str(list S, any mask, str variable_prefix="x", bool big_endian=true) str {
   "Return a component ANF as a Sage-style polynomial string."
   _sbox_terms_str_desc(sbox_component_anf(S, mask, big_endian), sbox_input_size(S), variable_prefix, big_endian)
}

fn sbox_polynomial_strs(list S, str variable_prefix="x", bool big_endian=true) list {
   "Return output-bit ANFs as Sage-style polynomial strings."
   def out_bits = sbox_output_size(S)
   mut out = []
   mut pos = 0
   while(pos < out_bits){
      def bit_pos = big_endian ? (out_bits - 1 - pos) : pos
      out = out.append(sbox_component_anf_str(S, 1 << bit_pos, variable_prefix, big_endian))
      pos += 1
   }
   out
}

fn _sbox_comb_masks_from(int total, int start, int need, int mask) list {
   if(need == 0){ return [mask] }
   mut out = []
   mut i = start
   while(i <= total - need){
      def tail = _sbox_comb_masks_from(total, i + 1, need - 1, mask | (1 << i))
      mut j = 0
      while(j < tail.len){
         out = out.append(tail.get(j))
         j += 1
      }
      i += 1
   }
   out
}

fn _sbox_monomial_masks_upto(int total, int degree) list {
   if(degree < 0){ panic("sbox degree fit: degree must be non-negative") }
   if(degree > total){ degree = total }
   mut out = []
   mut d = 0
   while(d <= degree){
      def masks = _sbox_comb_masks_from(total, 0, d, 0)
      mut i = 0
      while(i < masks.len){
         out = out.append(masks.get(i))
         i += 1
      }
      d += 1
   }
   out
}

fn _sbox_eval_graph_monomial(list S, int x, int mask, int in_bits, int out_bits, bool big_endian) int {
   mut prod = 1
   mut idx = 0
   while(idx < in_bits + out_bits){
      if(((mask >> idx) & 1) != 0){
         def bit = idx < in_bits ? _sbox_ordered_bit(x, idx, in_bits, big_endian) : _sbox_ordered_bit(S.get(x), idx - in_bits, out_bits, big_endian)
         prod = prod & bit
      }
      idx += 1
   }
   prod
}

fn _sbox_row_xor(list a, list b) list {
   mut out = []
   mut i = 0
   while(i < a.len){
      out = out.append(int(a.get(i)) ^^ int(b.get(i)))
      i += 1
   }
   out
}

fn _sbox_degree_fit_rows(list S, int degree, bool big_endian) dict {
   _sbox_check(S)
   def in_bits = sbox_input_size(S)
   def out_bits = sbox_output_size(S)
   def monomials = _sbox_monomial_masks_upto(in_bits + out_bits, degree)
   mut rows = []
   mut mi = 0
   while(mi < monomials.len){
      def mask = int(monomials.get(mi))
      mut row = []
      mut x = 0
      while(x < S.len){
         row = row.append(_sbox_eval_graph_monomial(S, x, mask, in_bits, out_bits, big_endian))
         x += 1
      }
      x = 0
      while(x < monomials.len){
         row = row.append(x == mi ? 1 : 0)
         x += 1
      }
      rows = rows.append(row)
      mi += 1
   }
   mut rank = 0
   mut col = 0
   while(col < S.len){
      mut pivot = -1
      mut r = rank
      while(r < rows.len){
         if(int(rows.get(r).get(col)) != 0 && pivot < 0){ pivot = r }
         r += 1
      }
      if(pivot >= 0){
         if(pivot != rank){
            def tmp = rows.get(rank)
            rows[rank] = rows.get(pivot)
            rows[pivot] = tmp
         }
         r = 0
         while(r < rows.len){
            if(r != rank && int(rows.get(r).get(col)) != 0){
               rows[r] = _sbox_row_xor(rows.get(r), rows.get(rank))
            }
            r += 1
         }
         rank += 1
      }
      col += 1
   }
   def rank_size = rank >= monomials.len ? (monomials.len - 1) : rank
   {"rows": rows, "rank": rank, "rank_size": rank_size, "monomials": monomials}
}

fn _sbox_graph_monomial_str(int mask, int in_bits, str x_prefix, str y_prefix) str {
   if(mask == 0){ return "1" }
   mut out = ""
   mut v = 0
   while(v < in_bits + 64){
      if(((mask >> v) & 1) != 0){
         def name = v < in_bits ? (x_prefix + to_str(v)) : (y_prefix + to_str(v - in_bits))
         out = out.len == 0 ? name : (out + "*" + name)
      }
      v += 1
      if((mask >> v) == 0 && v >= in_bits){ break }
   }
   out
}

fn _sbox_degree_fit_poly_str(list coeffs, list monomials, int in_bits, int out_bits, int degree, str x_prefix, str y_prefix) str {
   mut out = ""
   mut d = degree
   while(d >= 0){
      mut i = 0
      while(i < monomials.len){
         def mask = int(monomials.get(i))
         if(int(coeffs.get(i)) != 0 && bin.bit_count(mask) == d){
            def term = _sbox_graph_monomial_str(mask, in_bits, x_prefix, y_prefix)
            out = out.len == 0 ? term : (out + " + " + term)
         }
         i += 1
      }
      d -= 1
   }
   out.len == 0 ? "0" : out
}

fn sbox_degree_fit_polynomial_strs(list S, int degree=2, str x_prefix="x", str y_prefix="y", bool big_endian=true) list {
   "Return Sage-compatible default SBox.polynomials(degree=...) fitting relations."
   def fit = _sbox_degree_fit_rows(S, degree, big_endian)
   def rows = fit["rows"]
   def rank = int(fit.get("rank_size", fit["rank"]))
   def monomials = fit["monomials"]
   def in_bits = sbox_input_size(S)
   def out_bits = sbox_output_size(S)
   mut out = []
   mut r = rank
   while(r < rows.len){
      def row = rows.get(r)
      mut coeffs = []
      mut i = 0
      while(i < monomials.len){
         coeffs = coeffs.append(row.get(S.len + i))
         i += 1
      }
      out = out.append(_sbox_degree_fit_poly_str(coeffs, monomials, in_bits, out_bits, degree, x_prefix, y_prefix))
      r += 1
   }
   out
}

fn sbox_degree_fit_basis_report(list S, int degree=2, bool big_endian=true) dict {
   "Return an auditable report for Sage-compatible degree-fit S-box relations."
   def fit = _sbox_degree_fit_rows(S, degree, big_endian)
   def basis = sbox_degree_fit_polynomial_strs(S, degree, "x", "y", big_endian)
   {
      "basis": basis,
      "basis_size": basis.len,
      "rank": fit["rank"],
      "rank_size": fit.get("rank_size", fit["rank"]),
      "monomial_count": fit["monomials"].len,
      "degree": degree,
      "input_bits": sbox_input_size(S),
      "output_bits": sbox_output_size(S),
      "solution_count": S.len,
      "matches_sage_default_polynomials": true
   }
}

fn sbox_direct_polynomial_strs(list S, str x_prefix="x", str y_prefix="y", bool big_endian=true) list {
   "Return direct Boolean equations `yi + ANF_i(x)` as Sage-style strings."
   def out_bits = sbox_output_size(S)
   mut out = []
   mut pos = 0
   while(pos < out_bits){
      def bit_pos = big_endian ? (out_bits - 1 - pos) : pos
      def rhs = sbox_component_anf_str(S, 1 << bit_pos, x_prefix, big_endian)
      out = out.append(y_prefix + to_str(pos) + (rhs == "0" ? "" : (" + " + rhs)))
      pos += 1
   }
   out
}

fn sbox_groebner_polynomial_strs(list S, str x_prefix="x", str y_prefix="y", bool big_endian=true) list {
   "Return the reduced lex Boolean Groebner basis for the graph y = S(x), with field equations omitted."
   sbox_direct_polynomial_strs(S, x_prefix, y_prefix, big_endian)
}

fn sbox_groebner_basis_report(list S, str x_prefix="x", str y_prefix="y", bool big_endian=true) dict {
   "Return an auditable report for the reduced lex Boolean Groebner basis of y = S(x)."
   def basis = sbox_groebner_polynomial_strs(S, x_prefix, y_prefix, big_endian)
   {
      "basis": basis,
      "basis_size": basis.len,
      "input_bits": sbox_input_size(S),
      "output_bits": sbox_output_size(S),
      "solution_count": S.len,
      "variable_order": "lex-output-greater-than-input",
      "field_equations_filtered": true,
      "reduced": true,
      "direct_function_graph": true
   }
}

fn sbox_eval_anf(list monomials, int x) int {
   "Evaluate an ANF encoded as monomial masks at integer input x."
   mut out = 0
   mut i = 0
   while(i < monomials.len){
      def m = int(monomials.get(i))
      if((x & m) == m){ out = out ^^ 1 }
      i += 1
   }
   out
}

fn _sbox_clause_literal(int var_idx, int bit) int {
   bit != 0 ? (0 - var_idx) : var_idx
}

fn _sbox_required_literal(int var_idx, int bit) int {
   bit != 0 ? var_idx : (0 - var_idx)
}

fn _sbox_ordered_bit(int value, int pos, int width, bool big_endian) int {
   def bit_pos = big_endian ? (width - 1 - pos) : pos
   (value >> bit_pos) & 1
}

fn _sbox_default_indices(int start, int count) list {
   mut out = []
   mut i = 0
   while(i < count){
      out = out.append(start + i)
      i += 1
   }
   out
}

fn _sbox_validate_indices(any raw, int count, str name) list {
   if(raw == nil){ return _sbox_default_indices(name == "xi" ? 1 : 0, count) }
   if(!is_list(raw)){ panic("sbox_cnf: " + name + " must be a list") }
   if(raw.len != count){ panic("sbox_cnf: " + name + " has wrong length") }
   mut out = []
   mut i = 0
   while(i < raw.len){
      out = out.append(int(raw.get(i)))
      i += 1
   }
   out
}

fn _sbox_cnf_clauses_with_indices(list S, list xi, list yi, bool big_endian) list {
   def in_bits = sbox_input_size(S)
   def out_bits = sbox_output_size(S)
   mut output_order = []
   mut i = 0
   while(i < out_bits){
      output_order = output_order.append(big_endian ? i : (out_bits - 1 - i))
      i += 1
   }
   mut clauses = []
   mut x = 0
   while(x < S.len){
      def xbits = sbox_to_bits(S, x, in_bits, big_endian)
      def ybits = sbox_apply(S, xbits, big_endian)
      mut oi = 0
      while(oi < output_order.len){
         def output_bit = int(output_order.get(oi))
         mut clause = []
         i = 0
         while(i < in_bits){
            clause = clause.append(_sbox_clause_literal(int(xi.get(i)), int(xbits.get(i))))
            i += 1
         }
         clause = clause.append(_sbox_required_literal(int(yi.get(output_bit)), int(ybits.get(output_bit))))
         clauses = clauses.append(clause)
         oi += 1
      }
      x += 1
   }
   clauses
}

fn _sbox_cnf_dimacs(list clauses, int var_count, bool header) str {
   mut out = header ? ("p cnf " + to_str(var_count) + " " + to_str(clauses.len) + "\n") : ""
   mut i = 0
   while(i < clauses.len){
      def clause = clauses.get(i)
      mut j = 0
      while(j < clause.len){
         if(j > 0){ out = out + " " }
         out = out + to_str(clause.get(j))
         j += 1
      }
      out = out + " 0\n"
      i += 1
   }
   out
}

fn _sbox_symbolic_var(int idx, int in_bits) str {
   idx <= in_bits ? ("x" + to_str(idx - 1)) : ("y" + to_str(idx - in_bits - 1))
}

fn _sbox_cnf_symbolic(list clauses, int in_bits) str {
   mut out = ""
   mut i = 0
   while(i < clauses.len){
      if(i > 0){ out = out + " & " }
      def clause = clauses.get(i)
      out = out + "("
      mut j = 0
      while(j < clause.len){
         if(j > 0){ out = out + "|" }
         def lit = int(clause.get(j))
         if(lit < 0){
            out = out + "~" + _sbox_symbolic_var(0 - lit, in_bits)
         } else {
            out = out + _sbox_symbolic_var(lit, in_bits)
         }
         j += 1
      }
      out = out + ")"
      i += 1
   }
   out
}

fn _sbox_cnf_symbolic_sage_legacy(list clauses, int in_bits) str {
   mut out = ""
   mut i = 0
   while(i < clauses.len){
      if(i > 0){ out = out + " & " }
      def clause = clauses.get(i)
      out = out + "("
      mut j = 0
      while(j < clause.len){
         if(j > 0){ out = out + "|" }
         out = out + _sbox_symbolic_var(_sbox_abs_int(int(clause.get(j))), in_bits)
         j += 1
      }
      out = out + ")"
      i += 1
   }
   out
}

fn sbox_cnf(list S, any xi=nil, any yi=nil, any format=nil, bool big_endian=true) any {
   "Return Sage-compatible CNF clauses or formatted DIMACS/symbolic output for y = S(x)."
   _sbox_check(S)
   def in_bits = sbox_input_size(S)
   def out_bits = sbox_output_size(S)
   def xin = xi == nil ? _sbox_default_indices(1, in_bits) : _sbox_validate_indices(xi, in_bits, "xi")
   def yin = yi == nil ? _sbox_default_indices(in_bits + 1, out_bits) : _sbox_validate_indices(yi, out_bits, "yi")
   def clauses = _sbox_cnf_clauses_with_indices(S, xin, yin, big_endian)
   if(format == nil){ return clauses }
   def mode = to_str(format)
   if(mode == "dimacs"){ return _sbox_cnf_dimacs(clauses, in_bits + out_bits, true) }
   if(mode == "dimacs_headless"){ return _sbox_cnf_dimacs(clauses, in_bits + out_bits, false) }
   if(mode == "symbolic"){ return _sbox_cnf_symbolic(clauses, in_bits) }
   if(mode == "symbolic_sage" || mode == "symbolic_sage_legacy"){ return _sbox_cnf_symbolic_sage_legacy(clauses, in_bits) }
   panic("sbox_cnf: unsupported format " + mode)
}

fn sbox_cnf_clauses(list S, bool big_endian=true) list {
   "Return Sage-compatible CNF clauses for y = S(x)."
   sbox_cnf(S, nil, nil, nil, big_endian)
}

fn sbox_cnf_satisfied(list clauses, list assignment) bool {
   "Return true if a 0/1 assignment satisfies clauses returned by sbox_cnf_clauses."
   mut c = 0
   while(c < clauses.len){
      def clause = clauses.get(c)
      mut ok = false
      mut i = 0
      while(i < clause.len){
         def lit = int(clause.get(i))
         def idx = _sbox_abs_int(lit) - 1
         if(idx < 0 || idx >= assignment.len){ panic("sbox_cnf_satisfied: assignment is missing a variable") }
         def bit = int(assignment.get(idx)) & 1
         if((lit > 0 && bit != 0) || (lit < 0 && bit == 0)){ ok = true }
         i += 1
      }
      if(!ok){ return false }
      c += 1
   }
   true
}

fn sbox_autocorrelation_table(list S) any {
   "Return the Sage-style autocorrelation table for all input/output masks."
   _sbox_check(S)
   def in_n = S.len
   def out_n = _sbox_output_cardinality(S)
   mut rows = []
   mut a = 0
   while(a < in_n){
      mut row = []
      mut b = 0
      while(b < out_n){
         mut acc = 0
         mut x = 0
         while(x < in_n){
            def p = _sbox_parity(b & S.get(x)) ^^ _sbox_parity(b & S.get(x ^^ a))
            acc += p == 0 ? 1 : -1
            x += 1
         }
         row = row.append(acc)
         b += 1
      }
      rows = rows.append(row)
      a += 1
   }
   matrix.Matrix(rows)
}

fn sbox_boomerang_connectivity_table(list S) any {
   "Return the boomerang connectivity table for an invertible square S-box."
   if(!sbox_is_permutation(S)){ panic("sbox_boomerang_connectivity_table: S-box must be a permutation") }
   def inv = sbox_inverse(S)
   def n = S.len
   mut rows = list(n)
   __list_set_len(rows, n)
   mut dx = 0
   while(dx < n){
      __store_item_fast(rows, dx, _sbox_zero_row(n))
      dx += 1
   }
   mut dy = 0
   while(dy < n){
      mut shifted = list(n)
      __list_set_len(shifted, n)
      mut x = 0
      while(x < n){
         __store_item_fast(shifted, x, __load_item_fast(inv, __load_item_fast(S, x) ^^ dy))
         x += 1
      }
      x = 0
      while(x < n){
         def sx = __load_item_fast(shifted, x)
         mut y = x + 1
         while(y < n){
            def dx2 = x ^^ y
            def lhs = sx ^^ __load_item_fast(shifted, y)
            if(lhs >= dx2 && lhs <= dx2){
               __store_item_fast(__load_item_fast(rows, dx2), dy, __load_item_fast(__load_item_fast(rows, dx2), dy) + 2)
            }
            y += 1
         }
         x += 1
      }
      __store_item_fast(__load_item_fast(rows, 0), dy, n)
      dy += 1
   }
   matrix.Matrix(rows)
}

fn sbox_boomerang_uniformity(list S) int {
   "Return maximum BCT entry excluding the first row and column."
   if(!sbox_is_permutation(S)){ panic("sbox_boomerang_uniformity: S-box must be a permutation") }
   def inv = sbox_inverse(S)
   def n = S.len
   mut best = 0
   mut dy = 1
   while(dy < n){
      mut shifted = list(n)
      __list_set_len(shifted, n)
      mut x = 0
      while(x < n){
         __store_item_fast(shifted, x, __load_item_fast(inv, __load_item_fast(S, x) ^^ dy))
         x += 1
      }
      mut counts = _sbox_zero_row(n)
      x = 0
      while(x < n){
         def sx = __load_item_fast(shifted, x)
         mut y = x + 1
         while(y < n){
            def dx = x ^^ y
            def lhs = sx ^^ __load_item_fast(shifted, y)
            if(lhs >= dx && lhs <= dx){
               def c = __load_item_fast(counts, dx) + 2
               __store_item_fast(counts, dx, c)
               if(c > best){ best = c }
            }
            y += 1
         }
         x += 1
      }
      dy += 1
   }
   best
}

fn sbox_linear_structures(list S) list {
   "Return Sage-style [output_mask, input_mask, constant] linear structures."
   def act = sbox_autocorrelation_table(S)
   def out_n = _sbox_output_cardinality(S)
   mut out = []
   mut b = 1
   while(b < out_n){
      mut a = 1
      while(a < S.len){
         def v = matrix.mat_get(act, a, b)
         if(_sbox_abs_int(v) == S.len){
            def c = v == S.len ? 0 : 1
            out = out.append([b, a, c])
         }
         a += 1
      }
      b += 1
   }
   out
}

fn sbox_has_linear_structure(list S) bool {
   "Return true when a non-zero component has a non-zero linear structure."
   sbox_linear_structures(S).len > 0
}

fn sbox_is_linear_structure(list S, any a, any b, bool big_endian=true) bool {
   "Return true if a is a linear structure of component b dot S(x)."
   def ai = _sbox_input_mask_loose(S, a, big_endian)
   def bi = _sbox_component_mask(S, b, big_endian)
   if(ai == 0 || bi == 0){ return false }
   def act = sbox_autocorrelation_table(S)
   _sbox_abs_int(matrix.mat_get(act, ai, bi)) == S.len
}

fn sbox_is_apn(list S) bool {
   "Return true for square APN S-boxes."
   if(sbox_input_size(S) != sbox_output_size(S)){ panic("sbox_is_apn: APN is defined for square S-boxes") }
   sbox_differential_uniformity(S) == 2
}

fn sbox_is_balanced(list S) bool {
   "Return true when every non-zero component function is balanced."
   def out_n = _sbox_output_cardinality(S)
   mut b = 1
   while(b < out_n){
      mut ones = 0
      mut x = 0
      while(x < S.len){
         ones += _sbox_parity(b & S.get(x))
         x += 1
      }
      if(ones * 2 != S.len){ return false }
      b += 1
   }
   true
}

fn sbox_is_almost_bent(list S) bool {
   "Return true when a square odd-dimensional S-box has optimal AB nonlinearity."
   def m = sbox_input_size(S)
   if(m != sbox_output_size(S)){ panic("sbox_is_almost_bent: almost-bent is defined for square S-boxes") }
   if((m & 1) == 0){ return false }
   sbox_nonlinearity(S) == ((1 << (m - 1)) - (1 << ((m - 1) / 2)))
}

fn sbox_is_bent(list S) bool {
   "Return true when the S-box reaches the bent nonlinearity bound."
   def m = sbox_input_size(S)
   def n = sbox_output_size(S)
   if((m & 1) != 0 || n > m / 2){ return false }
   sbox_nonlinearity(S) == ((1 << (m - 1)) - (1 << (m / 2 - 1)))
}

fn sbox_is_plateaued(list S) bool {
   "Return true when every non-zero component has a plateaued Walsh spectrum."
   def lat = _sbox_walsh_table(S)
   def out_n = _sbox_output_cardinality(S)
   mut b = 1
   while(b < out_n){
      mut mag = 0
      mut a = 0
      while(a < S.len){
         def v = _sbox_abs_int(matrix.mat_get(lat, a, b))
         if(v != 0){
            if(mag == 0){ mag = v }
            elif(v != mag){ return false }
         }
         a += 1
      }
      b += 1
   }
   true
}

fn sbox_is_involution(list S) bool {
   "Return true if the S-box equals its inverse."
   if(!sbox_is_permutation(S)){ return false }
   def inv = sbox_inverse(S)
   mut i = 0
   while(i < S.len){
      if(inv.get(i) != S.get(i)){ return false }
      i += 1
   }
   true
}

fn _sbox_construction_check(list sboxes) int {
   if(sboxes.len == 0){ panic("sbox construction: no input S-boxes") }
   def width = sbox_input_size(sboxes.get(0))
   if(width != sbox_output_size(sboxes.get(0))){ panic("sbox construction: S-boxes must be square") }
   mut i = 1
   while(i < sboxes.len){
      if(sbox_input_size(sboxes.get(i)) != width || sbox_output_size(sboxes.get(i)) != width){
         panic("sbox construction: all S-boxes must share one square width")
      }
      i += 1
   }
   width
}

fn sbox_feistel_construction(list sboxes) list {
   "Return the Sage-style Feistel S-box construction over a list of round S-boxes."
   def w = _sbox_construction_check(sboxes)
   def mask = (1 << w) - 1
   def size = 1 << (2 * w)
   mut out = []
   mut x = 0
   while(x < size){
      mut xl = (x >> w) & mask
      mut xr = x & mask
      mut r = 0
      while(r < sboxes.len){
         def sb = sboxes.get(r)
         def next_l = sb.get(xl) ^^ xr
         xr = xl
         xl = next_l
         r += 1
      }
      out = out.append((xl << w) | xr)
      x += 1
   }
   out
}

fn sbox_misty_construction(list sboxes) list {
   "Return the Sage-style MISTY S-box construction over a list of round S-boxes."
   def w = _sbox_construction_check(sboxes)
   def mask = (1 << w) - 1
   def size = 1 << (2 * w)
   mut out = []
   mut x = 0
   while(x < size){
      mut xl = (x >> w) & mask
      mut xr = x & mask
      mut r = 0
      while(r < sboxes.len){
         def sb = sboxes.get(r)
         def next_l = sb.get(xr) ^^ xl
         xr = xl
         xl = next_l
         r += 1
      }
      out = out.append((xl << w) | xr)
      x += 1
   }
   out
}

fn sbox_differential_branch_number(list S) int {
   "Return min wt(x xor y)+wt(S[x] xor S[y]) for x != y."
   _sbox_check(S)
   mut best = S.len + _sbox_output_cardinality(S)
   mut dx = 1
   while(dx < S.len){
      def in_wt = bin.bit_count(dx)
      mut x = 0
      while(x < S.len){
         def score = in_wt + bin.bit_count(__load_item_fast(S, x) ^^ __load_item_fast(S, x ^^ dx))
         if(score < best){
            best = score
            if(best <= 1){ return best }
         }
         x += 1
      }
      dx += 1
   }
   best
}

fn sbox_linear_branch_number(list S) int {
   "Return min wt(a)+wt(b) over non-zero LAT entries with b non-zero."
   _sbox_check(S)
   def in_n = S.len
   def out_n = _sbox_output_cardinality(S)
   mut best = S.len + out_n
   mut b = 1
   while(b < out_n){
      mut vec = list(in_n)
      __list_set_len(vec, in_n)
      mut x = 0
      while(x < in_n){
         __store_item_fast(vec, x, (_sbox_parity(b & __load_item_fast(S, x)) == 0) ? 1 : -1)
         x += 1
      }
      mut step = 1
      while(step < in_n){
         mut base = 0
         while(base < in_n){
            x = 0
            while(x < step){
               def u = __load_item_fast(vec, base + x)
               def v = __load_item_fast(vec, base + x + step)
               __store_item_fast(vec, base + x, u + v)
               __store_item_fast(vec, base + x + step, u - v)
               x += 1
            }
            base += step * 2
         }
         step = step << 1
      }
      mut a = 0
      while(a < in_n){
         if(__load_item_fast(vec, a) != 0){
            def score = bin.bit_count(a) + bin.bit_count(b)
            if(score < best){ best = score }
         }
         a += 1
      }
      b += 1
   }
   best
}

fn sbox_branch_number(list S) int {
   "Alias for the differential branch number kept for compatibility."
   sbox_differential_branch_number(S)
}

fn sbox_max_degree(list S) int {
   "Return maximum algebraic degree over non-zero component functions."
   sbox_algebraic_degree(S)
}

fn sbox_min_degree(list S) int {
   "Return minimum algebraic degree over non-zero component functions."
   _sbox_check(S)
   def out_n = _sbox_output_cardinality(S)
   mut best = sbox_input_size(S)
   mut mask = 1
   while(mask < out_n){
      mut table = []
      mut x = 0
      while(x < S.len){
         table = table.append(_sbox_parity(mask & S.get(x)))
         x += 1
      }
      mut step = 1
      while(step < S.len){
         mut i = 0
         while(i < S.len){
            if((i & step) != 0){ table[i] = table.get(i) ^^ table.get(i ^^ step) }
            i += 1
         }
         step = step << 1
      }
      mut deg = 0
      mut idx = 0
      while(idx < S.len){
         if(table.get(idx) != 0){
            def d = bin.bit_count(idx)
            if(d > deg){ deg = d }
         }
         idx += 1
      }
      if(deg < best){ best = deg }
      mask += 1
   }
   best
}

fn sbox_algebraic_degree(list S) int {
   "Return the maximum algebraic normal form degree over output bits."
   _sbox_check(S)
   def n = S.len
   def out_bits = sbox_output_size(S)
   mut best = 0
   mut bit = 0
   while(bit < out_bits){
      mut coeff = list(n)
      __list_set_len(coeff, n)
      mut x = 0
      while(x < n){
         __store_item_fast(coeff, x, (__load_item_fast(S, x) >> bit) & 1)
         x += 1
      }
      mut step = 1
      while(step < n){
         mut mask = 0
         while(mask < n){
            if((mask & step) != 0){ __store_item_fast(coeff, mask, __load_item_fast(coeff, mask) ^^ __load_item_fast(coeff, mask ^^ step)) }
            mask += 1
         }
         step = step << 1
      }
      mut mask = 0
      while(mask < n){
         if(__load_item_fast(coeff, mask) != 0){
            def deg = bin.bit_count(mask)
            if(deg > best){ best = deg }
         }
         mask += 1
      }
      bit += 1
   }
   best
}

#main {
   def S = [0, 3, 1, 2]
   assert(sbox_is_permutation(S), "sbox permutation")
   assert(sbox_inverse(S) == [0, 2, 3, 1], "sbox inverse")
   assert(sbox_differential_uniformity(S) == 4, "sbox ddt uniformity")
   assert(sbox_linearity(S) == 4, "sbox Walsh linearity")
   assert(sbox_nonlinearity(S) == 0, "sbox nonlinearity")
   print("✓ std.math.crypto.symmetric.sbox self-test passed")
}
