;; Keywords: matrix linear-algebra math
;; Matrix construction and linear-algebra operations over numeric and modular values.
;; Full implementation with operator overloading and crypto primitives
;; References:
;; - std.math
module std.math.matrix(Matrix, is_matrix, mat_new, mat_get, mat_set, mat_mul, mat_det2,
   mat_print, mat_transpose, mat_identity, mat_add, mat_scale,
   mat4_zero, mat4_identity, mat4_get, mat4_set,
   __add, __sub, __mul, __pow, __neg, __eq, __neq, __str, __len,
   matrix_add, matrix_sub, matrix_mul, matrix_scale, matrix_neg, matrix_pow,
   matrix_transpose, matrix_mod, matrix_mul_mod, matrix_pow_mod, matrix_eq_mod,
   matrix_zero, matrix_one, matrix_identity, matrix_diagonal, matrix_random,
   matrix_det, matrix_trace, matrix_rank, matrix_rank_mod, matrix_nullity_mod,
   matrix_rref_mod, matrix_nullspace_mod, matrix_right_kernel_mod,
   matrix_left_kernel_mod, matrix_kernel_mod, matrix_inverse, matrix_adjugate,
   matrix_det_mod, matrix_determinantal_divisor, matrix_smith_invariants,
   matrix_elementary_divisors, matrix_smith_form, matrix_smith_normal_form,
   matrix_hermite_form, matrix_hermite_normal_form, matrix_hnf,
   matrix_is_hermite_form, matrix_hnf_transform, matrix_snf_transform,
   matrix_change_ring,
   matrix_solve, matrix_solve_mod, matrix_solve_right_mod, matrix_solve_left_mod,
   matrix_lu, matrix_gauss_eliminate,
_matrix_rows, _matrix_cols, _matrix_data, _matrix_get, _matrix_set)

use std.core
use std.core.str as str
use std.math.big
use std.math.integer (Z, gcd, mod, inverse_mod, xgcd)

fn is_matrix(any x) bool {
   "Check if x is a matrix."
   if(!is_ptr(x)){ return false }
   if(!is_list(x)){ return false }
   def n = x.len
   if(n < 3){ return false }
   def first = x.get(0)
   is_int(first)
}

fn _matrix_rows(any m) int { m.get(0) }

fn _matrix_cols(any m) int { m.get(1) }

fn _matrix_data(any m) list { m.get(2) }

fn _matrix_get(any m, int i, int j) any {
   def row = _matrix_data(m).get(i)
   row.get(j)
}

fn _matrix_set(any m, int i, int j, any val) any {
   def data = _matrix_data(m)
   def row = data.get(i)
   def new_row = row.set(j, val)
   def new_data = data.set(i, new_row)
   [_matrix_rows(m), _matrix_cols(m), new_data]
}

fn _matrix_make(int rows, int cols, any data) list { [rows, cols, data] }

fn mat_new(int rows, int cols, any init_val) list {
   "Create a plain matrix filled with `init_val`."
   mut data = list(0)
   mut i = 0
   while(i < rows){
      mut row = list(0)
      mut j = 0
      while(j < cols){
         row = row.append(init_val)
         j += 1
      }
      data = data.append(row)
      i += 1
   }
   _matrix_make(rows, cols, data)
}

fn mat_get(any mat, int row, int col) any {
   "Get matrix element at `(row, col)`."
   _matrix_get(mat, row, col)
}

fn mat_set(any mat, int row, int col, any val) any {
   "Set matrix element at `(row, col)`."
   _matrix_set(mat, row, col, val)
}

fn Matrix(list data) list {
   "Create matrix from list of lists."
   def rows = data.len
   if(rows == 0){ return [0, 0, list(0)] }
   def cols = len(data.get(0))
   mut i = 0
   while(i < rows){
      if(len(data.get(i)) != cols){ panic("Matrix: all rows must have same length") }
      i += 1
   }
   _matrix_make(rows, cols, data)
}

fn matrix_zero(int rows, int cols) any {
   "Create zero matrix."
   mut data = list(0)
   mut i = 0
   while(i < rows){
      mut row = list(0)
      mut j = 0
      while(j < cols){
         row = row.append(Z(0))
         j += 1
      }
      data = data.append(row)
      i += 1
   }
   _matrix_make(rows, cols, data)
}

fn matrix_one(int rows, int cols) any {
   "Create matrix of all ones."
   mut data = list(0)
   mut i = 0
   while(i < rows){
      mut row = list(0)
      mut j = 0
      while(j < cols){
         row = row.append(Z(1))
         j += 1
      }
      data = data.append(row)
      i += 1
   }
   _matrix_make(rows, cols, data)
}

fn matrix_identity(int n) any {
   "Create n x n identity matrix."
   mut data = list(0)
   mut i = 0
   while(i < n){
      mut row = list(0)
      mut j = 0
      while(j < n){
         if(i == j){ row = row.append(Z(1)) } else { row = row.append(Z(0)) }
         j += 1
      }
      data = data.append(row)
      i += 1
   }
   _matrix_make(n, n, data)
}

fn mat_identity(int n) any {
   "Create an identity matrix."
   matrix_identity(n)
}

fn mat4_zero() list {
   "Returns a zero-initialized flat row-major 4x4 matrix."
   [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
}

fn mat4_identity() list {
   "Returns a flat row-major 4x4 identity matrix."
   mut m = mat4_zero()
   m[0] = 1.0 m[5] = 1.0 m[10] = 1.0 m[15] = 1.0
   m
}

fn _mat4_offset(list m) int {
   def n = m.len
   if(n >= 18 && m[0] == 4 && m[1] == 4){ return 2 }
   if(n >= 16){ return 0 }
   -1
}

fn mat4_get(list m, int r, int c, f64 default=0.0) f64 {
   "Returns the flat row-major 4x4 matrix element at row `r`, column `c`, or `default` when out of bounds."
   if(r < 0 || r >= 4 || c < 0 || c >= 4){ return default }
   def off = _mat4_offset(m)
   if(off < 0){ return default }
   m[off + r * 4 + c]
}

fn mat4_set(list m, int r, int c, f64 v) list {
   "Stores `v` at row `r`, column `c` in a flat row-major 4x4 matrix and returns `m`."
   if(r < 0 || r >= 4 || c < 0 || c >= 4){ return m }
   def off = _mat4_offset(m)
   if(off < 0){ return m }
   m[off + r * 4 + c] = v
   m
}

fn matrix_diagonal(list diag) any {
   "Create diagonal matrix from list."
   def n = diag.len
   mut data = list(0)
   mut i = 0
   while(i < n){
      mut row = list(0)
      mut j = 0
      while(j < n){
         if(i == j){ row = row.append(diag.get(i)) } else { row = row.append(Z(0)) }
         j += 1
      }
      data = data.append(row)
      i += 1
   }
   _matrix_make(n, n, data)
}

fn matrix_random(int rows, int cols, any bound) any {
   "Create random matrix with entries in [0, bound)."
   def bound_big = (is_bigint(bound) ? bound : bigint(bound))
   mut data = list(0)
   mut i = 0
   while(i < rows){
      mut row = list(0)
      mut j = 0
      while(j < cols){
         row = row.append(bigint_random(bound_big))
         j += 1
      }
      data = data.append(row)
      i += 1
   }
   _matrix_make(rows, cols, data)
}

fn __add(any a, any b) any {
   if(!is_matrix(a) || !is_matrix(b)){ return nil }
   matrix_add(a, b)
}

fn __sub(any a, any b) any {
   if(!is_matrix(a) || !is_matrix(b)){ return nil }
   matrix_sub(a, b)
}

fn __mul(any a, any b) any {
   if(!is_matrix(a)){ return nil }
   if(is_bigint(b) || is_int(b)){ return matrix_scale(a, b) }
   if(is_matrix(b)){ return matrix_mul(a, b) }
   nil
}

fn __pow(any a, any b) any {
   if(!is_matrix(a)){ return nil }
   if(is_bigint(b)){
      def b_int = bigint_to_int(b)
      if(b_int < 0){ return nil }
      return matrix_pow(a, b_int)
   }
   if(is_int(b)){
      if(b < 0){ return nil }
      return matrix_pow(a, b)
   }
   nil
}

fn __neg(any a) any {
   if(!is_matrix(a)){ return nil }
   matrix_neg(a)
}

fn __eq(any a, any b) bool {
   if(!is_matrix(a) || !is_matrix(b)){ return false }
   if(_matrix_rows(a) != _matrix_rows(b)){ return false }
   if(_matrix_cols(a) != _matrix_cols(b)){ return false }
   mut i = 0
   while(i < _matrix_rows(a)){
      mut j = 0
      while(j < _matrix_cols(a)){
         if(!bigint_eq(_matrix_get(a, i, j), _matrix_get(b, i, j))){ return false }
         j += 1
      }
      i += 1
   }
   true
}

fn __neq(any a, any b) bool { !__eq(a, b) }

fn _matrix_str(any a) any {
   if(!is_matrix(a)){ return nil }
   mut b = str.Builder(128)
   b = str.builder_append(b, "[")
   mut i = 0
   while(i < _matrix_rows(a)){
      if(i > 0){ b = str.builder_append(b, ", ") }
      b = str.builder_append(b, "[")
      mut j = 0
      while(j < _matrix_cols(a)){
         if(j > 0){ b = str.builder_append(b, ", ") }
         b = str.builder_append(b, bigint_to_str(_matrix_get(a, i, j)))
         j += 1
      }
      b = str.builder_append(b, "]")
      i += 1
   }
   b = str.builder_append(b, "]")
   def s = str.builder_to_str(b)
   str.builder_free(b)
   s
}

fn __str(any a) any { _matrix_str(a) }

fn __len(any a) int {
   if(!is_matrix(a)){ return 0 }
   _matrix_rows(a)
}

fn matrix_add(any a, any b) any {
   "Add two matrices."
   if(_matrix_rows(a) != _matrix_rows(b) || _matrix_cols(a) != _matrix_cols(b)){ panic("matrix_add: dimension mismatch") }
   def rows = _matrix_rows(a)
   def cols = _matrix_cols(a)
   mut data = list(0)
   mut i = 0
   while(i < rows){
      mut row = list(0)
      mut j = 0
      while(j < cols){
         def sum = bigint_add(_matrix_get(a, i, j), _matrix_get(b, i, j))
         row = row.append(sum)
         j += 1
      }
      data = data.append(row)
      i += 1
   }
   _matrix_make(rows, cols, data)
}

fn mat_add(any a, any b) any {
   "Add two matrices."
   matrix_add(a, b)
}

fn matrix_sub(any a, any b) any {
   "Subtract two matrices."
   if(_matrix_rows(a) != _matrix_rows(b) || _matrix_cols(a) != _matrix_cols(b)){ panic("matrix_sub: dimension mismatch") }
   def rows = _matrix_rows(a)
   def cols = _matrix_cols(a)
   mut data = list(0)
   mut i = 0
   while(i < rows){
      mut row = list(0)
      mut j = 0
      while(j < cols){
         def diff = bigint_sub(_matrix_get(a, i, j), _matrix_get(b, i, j))
         row = row.append(diff)
         j += 1
      }
      data = data.append(row)
      i += 1
   }
   _matrix_make(rows, cols, data)
}

fn matrix_neg(any m) any {
   "Negate matrix."
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   mut data = list(0)
   mut i = 0
   while(i < rows){
      mut row = list(0)
      mut j = 0
      while(j < cols){
         row = row.append(bigint_neg(_matrix_get(m, i, j)))
         j += 1
      }
      data = data.append(row)
      i += 1
   }
   _matrix_make(rows, cols, data)
}

fn matrix_scale(any m, any c) any {
   "Scale matrix by constant."
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   def c_big = (is_bigint(c) ? c : bigint(c))
   mut data = list(0)
   mut i = 0
   while(i < rows){
      mut row = list(0)
      mut j = 0
      while(j < cols){
         row = row.append(bigint_mul(_matrix_get(m, i, j), c_big))
         j += 1
      }
      data = data.append(row)
      i += 1
   }
   _matrix_make(rows, cols, data)
}

fn mat_scale(any m, any c) any {
   "Scale a matrix by a scalar."
   matrix_scale(m, c)
}

fn matrix_mul(any a, any b) any {
   "Multiply two matrices."
   def rows_a = _matrix_rows(a)
   def cols_a = _matrix_cols(a)
   def rows_b = _matrix_rows(b)
   def cols_b = _matrix_cols(b)
   if(cols_a != rows_b){ panic("matrix_mul: dimension mismatch") }
   mut data = list(0)
   mut i = 0
   while(i < rows_a){
      mut row = list(0)
      mut j = 0
      while(j < cols_b){
         mut sum = Z(0)
         mut k = 0
         while(k < cols_a){
            def prod = bigint_mul(_matrix_get(a, i, k), _matrix_get(b, k, j))
            sum = bigint_add(sum, prod)
            k += 1
         }
         row = row.append(sum)
         j += 1
      }
      data = data.append(row)
      i += 1
   }
   _matrix_make(rows_a, cols_b, data)
}

fn mat_mul(any a, any b) any {
   "Multiply two matrices."
   matrix_mul(a, b)
}

fn matrix_pow(any m, int n) any {
   "Matrix power m^n using binary exponentiation."
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   if(rows != cols){ panic("matrix_pow: matrix must be square") }
   if(n == 0){ return matrix_identity(rows) }
   if(n == 1){ return m }
   mut result = matrix_identity(rows)
   mut base = m
   mut exp = n
   while(exp > 0){
      if(exp % 2 == 1){ result = matrix_mul(result, base) }
      base = matrix_mul(base, base)
      exp = exp / 2
   }
   result
}

fn matrix_transpose(any m) list {
   "Transpose matrix."
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   mut data = list(0)
   mut i = 0
   while(i < cols){
      mut row = list(0)
      mut j = 0
      while(j < rows){
         row = row.append(_matrix_get(m, j, i))
         j += 1
      }
      data = data.append(row)
      i += 1
   }
   _matrix_make(cols, rows, data)
}

fn mat_transpose(any m) any {
   "Transpose a matrix."
   matrix_transpose(m)
}

fn matrix_trace(any m) any {
   "Trace of square matrix(sum of diagonal)."
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   if(rows != cols){ panic("matrix_trace: matrix must be square") }
   mut sum = Z(0)
   mut i = 0
   while(i < rows){
      sum = bigint_add(sum, _matrix_get(m, i, i))
      i += 1
   }
   sum
}

fn matrix_det(any m) any {
   "Exact determinant using Bareiss fraction-free elimination."
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   if(rows != cols){ panic("matrix_det: matrix must be square") }
   if(rows == 0){ return Z(1) }
   if(rows == 1){ return _matrix_get(m, 0, 0) }
   if(rows == 2){
      def a, b = _matrix_get(m, 0, 0), _matrix_get(m, 0, 1)
      def c, d = _matrix_get(m, 1, 0), _matrix_get(m, 1, 1)
      return bigint_sub(bigint_mul(a, d), bigint_mul(b, c))
   }
   mut a, i = [], 0
   while(i < rows){
      mut row = []
      mut j = 0
      while(j < cols){
         row = row.append(Z(_matrix_get(m, i, j)))
         j += 1
      }
      a = a.append(row)
      i += 1
   }
   mut sign = Z(1)
   mut denom = Z(1)
   mut k = 0
   while(k < rows - 1){
      mut piv = k
      while(piv < rows && a.get(piv).get(k) == Z(0)){ piv += 1 }
      if(piv >= rows){ return Z(0) }
      if(piv != k){
         def tmp = a.get(k)
         a[k] = a.get(piv)
         a[piv] = tmp
         sign = -sign
      }
      def pivot = a.get(k).get(k)
      i = k + 1
      while(i < rows){
         mut j = k + 1
         while(j < rows){
            def v = (a.get(i).get(j) * pivot - a.get(i).get(k) * a.get(k).get(j)) / denom
            def rr = a.get(i)
            rr[j] = v
            a[i] = rr
            j += 1
         }
         def rr0 = a.get(i)
         rr0[k] = Z(0)
         a[i] = rr0
         i += 1
      }
      denom = pivot
      k += 1
   }
   sign * a.get(rows - 1).get(rows - 1)
}

fn matrix_det_mod(any m, any modn) any {
   "Determinant modulo `modn` using modular Gaussian elimination.
   Notes:
   - Works in Z/modnZ, so it needs modular inverses for pivots.
   - For composite moduli, a pivot might not be invertible. In that case we
   return nil so callers can fall back to an integer determinant if needed."
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   if(rows != cols){ panic("matrix_det_mod: matrix must be square") }
   if(rows == 0){ return Z(1) }
   if(rows == 1){ return mod(_matrix_get(m, 0, 0), modn) }
   mut a, i = [], 0
   while(i < rows){
      mut row = []
      mut j = 0
      while(j < cols){
         row = row.append(mod(_matrix_get(m, i, j), modn))
         j += 1
      }
      a = a.append(row)
      i += 1
   }
   mut det = Z(1)
   mut sign = 0
   i = 0
   while(i < rows){
      mut piv = i
      while(piv < rows && a.get(piv).get(i) == Z(0)){ piv += 1 }
      if(piv >= rows){ return Z(0) }
      if(piv != i){
         def tmp = a.get(i)
         a[i] = a.get(piv)
         a[piv] = tmp
         sign = 1 - sign
      }
      def pivot = a.get(i).get(i)
      def invp = inverse_mod(pivot, modn)
      if(invp == nil){
         return nil
      }
      det = mod(det * pivot, modn)
      mut r = i + 1
      while(r < rows){
         def factor = mod(a.get(r).get(i) * invp, modn)
         if(factor != Z(0)){
            mut c = i
            while(c < cols){
               def v = mod(a.get(r).get(c) - factor * a.get(i).get(c), modn)
               def rr = a.get(r)
               rr[c] = v
               a[r] = rr
               c += 1
            }
         }
         r += 1
      }
      i += 1
   }
   if(sign == 1){ det = mod(-det, modn) }
   det
}

fn mat_det2(any m) any {
   "Determinant of a 2x2 matrix."
   matrix_det(m)
}

fn mat_print(any m) any {
   "Print matrix row by row."
   print(_matrix_str(m))
   m
}

fn matrix_rank(any m) int {
   "Rank using Gaussian elimination."
   def lu_result = matrix_lu(m)
   def U = lu_result.get(1)
   def rows = _matrix_rows(U)
   def cols = _matrix_cols(U)
   mut rank = 0
   mut i = 0
   while(i < rows){
      mut is_zero = true
      mut j = 0
      while(j < cols){
         if(!bigint_eq(_matrix_get(U, i, j), Z(0))){
            is_zero = false
            break
         }
         j += 1
      }
      if(!is_zero){ rank += 1 }
      i += 1
   }
   rank
}

fn matrix_inverse(any m) any {
   "Matrix inverse using adjugate method."
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   if(rows != cols){ panic("matrix_inverse: matrix must be square") }
   def det = matrix_det(m)
   if(bigint_eq(det, Z(0))){ panic("matrix_inverse: singular matrix") }
   def adj = matrix_adjugate(m)
   matrix_scale(adj, bigint_div(Z(1), det))
}

fn matrix_adjugate(any m) any {
   "Adjugate(classical adjoint) matrix."
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   if(rows != cols){ panic("matrix_adjugate: matrix must be square") }
   if(rows == 1){ return matrix_identity(1) }
   if(rows == 2){
      def a, b = _matrix_get(m, 0, 0), _matrix_get(m, 0, 1)
      def c, d = _matrix_get(m, 1, 0), _matrix_get(m, 1, 1)
      return Matrix([[d, bigint_neg(b)], [bigint_neg(c), a]])
   }
   mut cofactor_data = list(0)
   mut i = 0
   while(i < rows){
      mut row = list(0)
      mut j = 0
      while(j < cols){
         def minor = _matrix_minor(m, i, j)
         def cofactor_sign = ((i + j) % 2 == 0 ? Z(1) : bigint_from_int(-1))
         def cofactor = bigint_mul(minor, cofactor_sign)
         row = row.append(cofactor)
         j += 1
      }
      cofactor_data = cofactor_data.append(row)
      i += 1
   }
   def cofactor = _matrix_make(rows, cols, cofactor_data)
   matrix_transpose(cofactor)
}

fn _matrix_minor(any m, int row, int col) any {
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   mut sub_data = list(0)
   mut i = 0
   while(i < rows){
      if(i == row){
         i += 1
         continue
      }
      mut sub_row = list(0)
      mut j = 0
      while(j < cols){
         if(j == col){
            j += 1
            continue
         }
         sub_row = sub_row.append(_matrix_get(m, i, j))
         j += 1
      }
      sub_data = sub_data.append(sub_row)
      i += 1
   }
   def sub = _matrix_make(rows - 1, cols - 1, sub_data)
   matrix_det(sub)
}

fn _matrix_submatrix_det(any m, list row_idx, list col_idx) any {
   def k = row_idx.len
   mut data = []
   mut i = 0
   while(i < k){
      mut row = []
      mut j = 0
      while(j < k){
         row = row.append(_matrix_get(m, int(row_idx[i]), int(col_idx[j])))
         j += 1
      }
      data = data.append(row)
      i += 1
   }
   matrix_det(Matrix(data))
}

fn _matrix_first_combination(int k) list {
   mut out = []
   mut i = 0
   while(i < k){
      out = out.append(i)
      i += 1
   }
   out
}

fn _matrix_next_combination(list comb, int n) any {
   def k = comb.len
   mut out = clone(comb)
   mut i = k - 1
   while(i >= 0){
      if(out[i] < n - k + i){
         out[i] = out[i] + 1
         mut j = i + 1
         while(j < k){
            out[j] = out[j - 1] + 1
            j += 1
         }
         return out
      }
      i -= 1
   }
   nil
}

fn matrix_determinantal_divisor(any m, int k) any {
   "Return the gcd of all k x k minors of an integer matrix.
   By convention the 0th determinantal divisor is 1. Returns 0 when all
   k-minors vanish or k is outside the matrix shape."
   if(k == 0){ return Z(1) }
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   if(k < 0 || k > rows || k > cols){ return Z(0) }
   mut g = Z(0)
   mut rs = _matrix_first_combination(k)
   while(rs != nil){
      mut cs = _matrix_first_combination(k)
      while(cs != nil){
         def d = bigint_abs(_matrix_submatrix_det(m, rs, cs))
         if(d != Z(0)){ if(g == Z(0)){ g = d } else { g = gcd(g, d) } }
         cs = _matrix_next_combination(cs, cols)
      }
      rs = _matrix_next_combination(rs, rows)
   }
   g
}

fn matrix_smith_invariants(any m) list {
   "Return nonzero Smith invariant factors of an integer matrix.
   This Sage-style surface computes determinantal divisors, so it is exact and
   dependency-free, but intended for small/medium crypto matrices rather than
   huge dense integer matrices."
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   def lim = rows < cols ? rows : cols
   mut invs = []
   mut prev = Z(1)
   mut k = 1
   while(k <= lim){
      def delta = matrix_determinantal_divisor(m, k)
      if(delta == Z(0)){ break }
      def di = bigint_div(delta, prev)
      invs = invs.append(bigint_abs(di))
      prev = delta
      k += 1
   }
   invs
}

fn matrix_elementary_divisors(any m) list {
   "Alias for `matrix_smith_invariants`."
   matrix_smith_invariants(m)
}

fn matrix_smith_form(any m) list {
   "Return a diagonal Smith normal form matrix with the same shape as `m`.
   Transformation matrices are intentionally not returned yet."
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   def invs = matrix_smith_invariants(m)
   mut data = []
   mut i = 0
   while(i < rows){
      mut row = []
      mut j = 0
      while(j < cols){
         if(i == j && i < invs.len){ row = row.append(invs[i]) } else { row = row.append(Z(0)) }
         j += 1
      }
      data = data.append(row)
      i += 1
   }
   Matrix(data)
}

fn matrix_smith_normal_form(any m) any {
   "Alias for `matrix_smith_form`."
   matrix_smith_form(m)
}

fn _matrix_row_linear(list a, list b, any ca, any cb) list {
   def cols = a.len
   mut out = []
   mut j = 0
   while(j < cols){
      out = out.append(ca * a.get(j) + cb * b.get(j))
      j += 1
   }
   out
}

fn _matrix_floor_div_pos(any a, any b) any {
   "Return floor(a / b) for positive integer b."
   def bb = Z(b)
   if(bb <= Z(0)){ panic("_matrix_floor_div_pos: divisor must be positive") }
   def aa = Z(a)
   def r = mod(aa, bb)
   (aa - r) / bb
}

fn _matrix_hermite_form_impl(any m) any {
   "Return a row-style integer Hermite normal form.
   The result is row-unimodularly equivalent to `m`: pivot columns increase
   from top to bottom, pivots are positive, entries below pivots are zero, and
   entries above each pivot are reduced into [0, pivot). This mirrors the
   Sage-style `hermite_form()` surface used by lattice and exact linear algebra workflows."
   mut src = m
   mut nrows = 0
   mut ncols = 0
   if(is_matrix(m)){
      nrows = int(m.get(0))
      ncols = int(m.get(1))
      src = m.get(2)
   } else {
      nrows = len(m)
      ncols = nrows > 0 ? len(m.get(0)) : 0
   }
   mut rows_data = []
   mut ii = 0
   while(ii < nrows){
      mut row_copy = []
      mut jj = 0
      while(jj < ncols){
         row_copy = row_copy.append(Z(src.get(ii).get(jj)))
         jj += 1
      }
      rows_data = rows_data.append(row_copy)
      ii += 1
   }
   mut prow = 0
   mut col = 0
   while(col < ncols && prow < nrows){
      mut pivot_row = -1
      mut i = prow
      while(i < nrows && pivot_row < 0){
         if(rows_data.get(i).get(col) != Z(0)){ pivot_row = i }
         i += 1
      }
      if(pivot_row >= 0){
         if(pivot_row != prow){
            def tmp = rows_data.get(prow)
            rows_data[prow] = rows_data.get(pivot_row)
            rows_data[pivot_row] = tmp
         }
         mut changed = true
         while(changed){
            changed = false
            i = prow + 1
            while(i < nrows && !changed){
               if(rows_data.get(i).get(col) != Z(0)){
                  def rp, ri = rows_data.get(prow), rows_data.get(i)
                  def aa = rp.get(col)
                  def bb = ri.get(col)
                  mut eg = xgcd(aa, bb)
                  mut g = eg.get(0)
                  mut s = eg.get(1)
                  mut t = eg.get(2)
                  if(g < Z(0)){
                     g, s = 0 - g, 0 - s
                     t = 0 - t
                  }
                  rows_data[prow] = _matrix_row_linear(rp, ri, s, t)
                  rows_data[i] = _matrix_row_linear(rp, ri, (0 - bb) / g, aa / g)
                  changed = true
               }
               i += 1
            }
         }
         if(rows_data.get(prow).get(col) < Z(0)){
            mut row = []
            mut j = 0
            while(j < ncols){
               row = row.append(0 - rows_data.get(prow).get(j))
               j += 1
            }
            rows_data[prow] = row
         }
         def pivot = rows_data.get(prow).get(col)
         i = 0
         while(i < prow){
            def q = _matrix_floor_div_pos(rows_data.get(i).get(col), pivot)
            if(q != Z(0)){ rows_data[i] = _matrix_row_linear(rows_data.get(i), rows_data.get(prow), Z(1), 0 - q) }
            i += 1
         }
         prow += 1
      }
      col += 1
   }
   def out_matrix = [nrows, ncols, rows_data]
   return out_matrix
}

fn matrix_hermite_form(any m) list {
   "Return a row-style integer Hermite normal form.
   The result is row-unimodularly equivalent to `m`: pivot columns increase
   from top to bottom, pivots are positive, entries below pivots are zero, and
   entries above each pivot are reduced into [0, pivot). This mirrors the
   Sage-style `hermite_form()` surface used by lattice and exact linear algebra workflows."
   def raw = _matrix_hermite_form_impl(m)
   if(is_matrix(raw)){ return raw }
   def rows = len(raw)
   def cols = rows > 0 ? len(raw.get(0)) : 0
   return [rows, cols, raw]
}

fn matrix_hermite_normal_form(any m) list {
   "Alias for `matrix_hermite_form`."
   return matrix_hermite_form(m)
}

fn matrix_hnf(any m) list {
   "Short Sage-style alias for `matrix_hermite_form`."
   return matrix_hermite_form(m)
}

fn matrix_hnf_transform(any m) any {
   "Reserved transform-returning HNF surface. Returns [H, U] once unimodular transforms are implemented."
   panic("matrix_hnf_transform: unimodular transform matrix is not implemented")
}

fn matrix_snf_transform(any m) any {
   "Reserved transform-returning SNF surface. Returns [S, U, V] once unimodular transforms are implemented."
   panic("matrix_snf_transform: unimodular transform matrices are not implemented")
}

fn matrix_change_ring(any m, any ring) list {
   "Return a copy of matrix `m` with entries normalized into a target scalar ring.
   Supports integer normalization, GF(p), and Zmod(n)-style modular rings."
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   mut kind = "zz"
   mut modulus = nil
   if(is_dict(ring)){
      def tag = ring.get("__type", "")
      if(tag == "gf"){
         kind = "mod"
         modulus = ring.get("p", nil)
      } elif(tag == "zmod_ring"){
         kind = "mod"
         modulus = ring.get("modulus", nil)
      }
   }
   if(is_int(ring) || is_bigint(ring)){
      kind = "mod"
      modulus = ring
   }
   if(is_str(ring) && (ring == "ZZ" || ring == "IntegerRing" || ring == "Integer Ring")){ kind = "zz" }
   mut data = []
   mut i = 0
   while(i < rows){
      mut row = []
      mut j = 0
      while(j < cols){
         def v = _matrix_get(m, i, j)
         row = row.append(kind == "mod" ? mod(v, modulus) : Z(v))
         j += 1
      }
      data = data.append(row)
      i += 1
   }
   Matrix(data)
}

fn matrix_is_hermite_form(any m) bool {
   "Return true when `m` satisfies the row Hermite normal form shape."
   mut src = m
   mut rows = 0
   mut cols = 0
   if(is_matrix(m)){
      rows = int(m.get(0))
      cols = int(m.get(1))
      src = m.get(2)
   } else {
      rows = len(m)
      cols = rows > 0 ? len(m.get(0)) : 0
   }
   mut last_pivot = -1
   mut zero_tail = false
   mut r = 0
   while(r < rows){
      mut pc = -1
      mut c = 0
      while(c < cols && pc < 0){
         if(!bigint_eq(Z(src.get(r).get(c)), Z(0))){ pc = c }
         c += 1
      }
      if(pc < 0){
         zero_tail = true
      } else {
         if(zero_tail){ return false }
         if(pc <= last_pivot){ return false }
         def pivot = Z(src.get(r).get(pc))
         if(bigint_le(pivot, Z(0))){ return false }
         mut rr = r + 1
         while(rr < rows){
            if(!bigint_eq(Z(src.get(rr).get(pc)), Z(0))){ return false }
            rr += 1
         }
         rr = 0
         while(rr < r){
            def v = Z(src.get(rr).get(pc))
            if(bigint_lt(v, Z(0)) || bigint_ge(v, pivot)){ return false }
            rr += 1
         }
         last_pivot = pc
      }
      r += 1
   }
   true
}

fn matrix_lu(any m) list {
   "LU decomposition with partial pivoting. Returns [L, U, swaps]."
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   mut U_data = list(0)
   mut i = 0
   while(i < rows){
      mut row = list(0)
      mut j = 0
      while(j < cols){
         row = row.append(_matrix_get(m, i, j))
         j += 1
      }
      U_data = U_data.append(row)
      i += 1
   }
   mut L_data = list(0)
   i = 0
   while(i < rows){
      mut row = list(0)
      mut j = 0
      while(j < cols){
         if(i == j){ row = row.append(Z(1)) } else { row = row.append(Z(0)) }
         j += 1
      }
      L_data = L_data.append(row)
      i += 1
   }
   mut swaps = 0
   mut k = 0
   while(k < rows - 1){
      mut max_val = Z(0)
      mut max_row = k
      i = k
      while(i < rows){
         def val = bigint_abs(_matrix_get_from_data(U_data, i, k))
         if(bigint_gt(val, max_val)){
            max_val = val
            max_row = i
         }
         i += 1
      }
      if(max_row != k){
         mut temp = U_data.get(k)
         U_data = U_data.set(k, U_data.get(max_row))
         U_data = U_data.set(max_row, temp)
         if(k > 0){
            temp = slice(L_data.get(k), 0, k)
            def temp2 = slice(L_data.get(max_row), 0, k)
            mut j = 0
            while(j < k){
               L_data = L_data.set(k, L_data.get(k).set(j, temp2.get(j)))
               L_data = L_data.set(max_row, L_data.get(max_row).set(j, temp.get(j)))
               j += 1
            }
         }
         swaps += 1
      }
      i = k + 1
      while(i < rows){
         def pivot = _matrix_get_from_data(U_data, k, k)
         if(!bigint_eq(pivot, Z(0))){
            def factor = bigint_div(_matrix_get_from_data(U_data, i, k), pivot)
            L_data = L_data.set(i, L_data.get(i).set(k, factor))
            mut j = k
            while(j < cols){
               def new_val = bigint_sub(_matrix_get_from_data(U_data, i, j),
               bigint_mul(factor, _matrix_get_from_data(U_data, k, j)))
               U_data = U_data.set(i, U_data.get(i).set(j, new_val))
               j += 1
            }
         }
         i += 1
      }
      k += 1
   }
   [_matrix_make(rows, cols, L_data), _matrix_make(rows, cols, U_data), swaps]
}

fn _matrix_get_from_data(list data, int i, int j) any { data.get(i).get(j) }

fn _matrix_modp(any x, any p) any {
   def r = x % p
   if(r < 0){ return r + p }
   r
}

fn _matrix_clone_mod_data(any m, any p) list {
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   mut out = []
   mut i = 0
   while(i < rows){
      mut row = []
      mut j = 0
      while(j < cols){
         row = row.append(_matrix_modp(_matrix_get(m, i, j), p))
         j += 1
      }
      out = out.append(row)
      i += 1
   }
   out
}

fn matrix_mod(any m, any p) any {
   "Reduce every matrix entry modulo `p`."
   _matrix_make(_matrix_rows(m), _matrix_cols(m), _matrix_clone_mod_data(m, p))
}

fn matrix_eq_mod(any a, any b, any p) bool {
   "Return true when matrices `a` and `b` are equal over Z/pZ."
   if(_matrix_rows(a) != _matrix_rows(b) || _matrix_cols(a) != _matrix_cols(b)){ return false }
   mut i = 0
   while(i < _matrix_rows(a)){
      mut j = 0
      while(j < _matrix_cols(a)){
         if(_matrix_modp(_matrix_get(a, i, j), p) != _matrix_modp(_matrix_get(b, i, j), p)){ return false }
         j += 1
      }
      i += 1
   }
   true
}

fn matrix_mul_mod(any a, any b, any p) any {
   "Multiply matrices over Z/pZ."
   def rows_a = _matrix_rows(a)
   def cols_a = _matrix_cols(a)
   def rows_b = _matrix_rows(b)
   def cols_b = _matrix_cols(b)
   if(cols_a != rows_b){ panic("matrix_mul_mod: dimension mismatch") }
   mut data = []
   mut i = 0
   while(i < rows_a){
      mut row = []
      mut j = 0
      while(j < cols_b){
         mut acc = 0
         mut k = 0
         while(k < cols_a){
            acc = _matrix_modp(acc + _matrix_get(a, i, k) * _matrix_get(b, k, j), p)
            k += 1
         }
         row = row.append(acc)
         j += 1
      }
      data = data.append(row)
      i += 1
   }
   _matrix_make(rows_a, cols_b, data)
}

fn matrix_pow_mod(any m, any e, any p) any {
   "Raise a square matrix to exponent `e` over Z/pZ."
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   if(rows != cols){ panic("matrix_pow_mod: matrix must be square") }
   mut ee = Z(e)
   if(ee < Z(0)){ panic("matrix_pow_mod: negative exponent is not supported") }
   mut result = matrix_mod(matrix_identity(rows), p)
   mut base = matrix_mod(m, p)
   while(ee > Z(0)){
      if(ee % Z(2) == Z(1)){ result = matrix_mul_mod(result, base, p) }
      base = matrix_mul_mod(base, base, p)
      ee = ee / Z(2)
   }
   result
}

fn matrix_rref_mod(any m, any p) list {
   "Reduced row-echelon form over GF(p). Returns `[rref_matrix, pivot_columns]`.
   The modulus `p` must be prime, or at least every selected pivot must be invertible."
   if(p <= 1){ panic("matrix_rref_mod: modulus must be > 1") }
   def rows = _matrix_rows(m)
   def cols = _matrix_cols(m)
   mut A = _matrix_clone_mod_data(m, p)
   mut pivots = []
   mut rank = 0
   mut col = 0
   while(col < cols && rank < rows){
      mut pivot = rank
      while(pivot < rows && _matrix_modp(A[pivot][col], p) == 0){ pivot += 1 }
      if(pivot >= rows){
         col += 1
         continue
      }
      if(pivot != rank){
         def tmp = A[rank]
         A[rank] = A[pivot]
         A[pivot] = tmp
      }
      def pivot_val = _matrix_modp(A[rank][col], p)
      def inv = inverse_mod(pivot_val, p)
      if(inv == nil){ panic("matrix_rref_mod: non-invertible pivot; use a prime modulus") }
      mut pivot_row = A[rank]
      mut j = col
      while(j < cols){
         pivot_row[j] = _matrix_modp(pivot_row[j] * inv, p)
         j += 1
      }
      A[rank] = pivot_row
      mut r = 0
      while(r < rows){
         if(r != rank){
            def factor = _matrix_modp(A[r][col], p)
            if(factor != 0){
               mut row_r = A[r]
               j = col
               while(j < cols){
                  row_r[j] = _matrix_modp(row_r[j] - factor * pivot_row[j], p)
                  j += 1
               }
               A[r] = row_r
            }
         }
         r += 1
      }
      pivots = pivots.append(col)
      rank += 1
      col += 1
   }
   [_matrix_make(rows, cols, A), pivots]
}

fn matrix_rank_mod(any m, any p) int {
   "Rank of `m` over GF(p)."
   matrix_rref_mod(m, p).get(1).len
}

fn matrix_nullity_mod(any m, any p) int {
   "Dimension of the right nullspace of `m` over GF(p)."
   _matrix_cols(m) - matrix_rank_mod(m, p)
}

fn matrix_nullspace_mod(any m, any p) list {
   "Basis for the right nullspace `{x | m*x = 0}` over GF(p)."
   def rr = matrix_rref_mod(m, p)
   def R = rr.get(0)
   def pivots = rr.get(1)
   def cols = _matrix_cols(m)
   mut is_pivot = []
   mut i = 0
   while(i < cols){
      is_pivot = is_pivot.append(false)
      i += 1
   }
   i = 0
   while(i < pivots.len){
      is_pivot[pivots.get(i)] = true
      i += 1
   }
   mut basis = []
   mut free_col = 0
   while(free_col < cols){
      if(!is_pivot.get(free_col)){
         mut v = []
         i = 0
         while(i < cols){
            v = v.append(0)
            i += 1
         }
         v[free_col] = 1
         mut row = 0
         while(row < pivots.len){
            def pc = pivots.get(row)
            v[pc] = _matrix_modp(0 - _matrix_get(R, row, free_col), p)
            row += 1
         }
         basis = basis.append(v)
      }
      free_col += 1
   }
   basis
}

fn matrix_right_kernel_mod(any m, any p) list {
   "Alias for `matrix_nullspace_mod`."
   matrix_nullspace_mod(m, p)
}

fn matrix_kernel_mod(any m, any p) list {
   "Alias for `matrix_nullspace_mod`."
   matrix_nullspace_mod(m, p)
}

fn matrix_left_kernel_mod(any m, any p) list {
   "Basis for the left nullspace `{x | x*m = 0}` over GF(p)."
   matrix_nullspace_mod(matrix_transpose(m), p)
}

fn matrix_solve_mod(any A, list b, any p) any {
   "Solve `A*x = b` over GF(p).
   Returns one solution vector with free variables set to zero, or nil when the
   system is inconsistent. The modulus `p` must be prime, or at least every
   selected pivot must be invertible."
   if(p <= 1){ panic("matrix_solve_mod: modulus must be > 1") }
   def rows = _matrix_rows(A)
   def cols = _matrix_cols(A)
   if(b.len != rows){ panic("matrix_solve_mod: right-hand side length mismatch") }
   mut aug = []
   mut i = 0
   while(i < rows){
      mut row = []
      mut j = 0
      while(j < cols){
         row = row.append(_matrix_modp(_matrix_get(A, i, j), p))
         j += 1
      }
      row = row.append(_matrix_modp(b.get(i), p))
      aug = aug.append(row)
      i += 1
   }
   def rr = matrix_rref_mod(Matrix(aug), p)
   def R = rr.get(0)
   def pivots = rr.get(1)
   i = 0
   while(i < pivots.len){
      if(pivots.get(i) == cols){ return nil }
      i += 1
   }
   mut x = []
   mut col = 0
   while(col < cols){
      mut value = 0
      mut row = 0
      while(row < pivots.len){
         if(pivots.get(row) == col){
            value = _matrix_modp(_matrix_get(R, row, cols), p)
            row = pivots.len
         } else {
            row += 1
         }
      }
      x = x.append(value)
      col += 1
   }
   x
}

fn matrix_solve_right_mod(any A, list b, any p) any {
   "Sage-style alias for solving `A*x = b` over GF(p)."
   matrix_solve_mod(A, b, p)
}

fn matrix_solve_left_mod(any A, list b, any p) any {
   "Solve `x*A = b` over GF(p)."
   matrix_solve_mod(matrix_transpose(A), b, p)
}

fn matrix_solve(any A, list b) list {
   "Solve Ax = b using LU decomposition."
   def lu_result = matrix_lu(A)
   def L = lu_result.get(0)
   def U = lu_result.get(1)
   def n = _matrix_rows(A)
   mut y, i = list(0), 0
   while(i < n){
      mut sum = Z(0)
      mut j = 0
      while(j < i){
         sum = bigint_add(sum, bigint_mul(_matrix_get(L, i, j), y.get(j)))
         j += 1
      }
      def yi = bigint_sub(b.get(i), sum)
      y = y.append(yi)
      i += 1
   }
   mut x = list(0)
   i = 0
   while(i < n){
      x = x.append(Z(0))
      i += 1
   }
   i = n - 1
   while(i >= 0){
      mut sum = Z(0)
      mut j = i + 1
      while(j < n){
         sum = bigint_add(sum, bigint_mul(_matrix_get(U, i, j), x.get(j)))
         j += 1
      }
      def xi = bigint_div(bigint_sub(y.get(i), sum), _matrix_get(U, i, i))
      x = x.set(i, xi)
      i -= 1
   }
   x
}

fn matrix_gauss_eliminate(any A, list b) list {
   "Gaussian elimination to solve Ax = b. Returns [A', b']."
   def rows = _matrix_rows(A)
   def cols = _matrix_cols(A)
   mut aug_data = list(0)
   mut i = 0
   while(i < rows){
      mut row = list(0)
      mut j = 0
      while(j < cols){
         row = row.append(_matrix_get(A, i, j))
         j += 1
      }
      row = row.append(b.get(i))
      aug_data = aug_data.append(row)
      i += 1
   }
   mut k = 0
   while(k < rows){
      mut max_row = k
      i = k + 1
      while(i < rows){
         if(bigint_gt(bigint_abs(_matrix_get_from_data(aug_data, i, k)),
            bigint_abs(_matrix_get_from_data(aug_data, max_row, k)))){
            max_row = i
         }
         i += 1
      }
      if(max_row != k){
         mut temp = aug_data.get(k)
         aug_data = aug_data.set(k, aug_data.get(max_row))
         aug_data = aug_data.set(max_row, temp)
      }
      i = k + 1
      while(i < rows){
         def pivot = _matrix_get_from_data(aug_data, k, k)
         if(!bigint_eq(pivot, Z(0))){
            def factor = bigint_div(_matrix_get_from_data(aug_data, i, k), pivot)
            mut j = k
            while(j <= cols){
               def new_val = bigint_sub(_matrix_get_from_data(aug_data, i, j),
               bigint_mul(factor, _matrix_get_from_data(aug_data, k, j)))
               aug_data = aug_data.set(i, aug_data.get(i).set(j, new_val))
               j += 1
            }
         }
         i += 1
      }
      k += 1
   }
   mut A_data = list(0)
   mut b_vec = list(0)
   i = 0
   while(i < rows){
      mut row = list(0)
      mut j = 0
      while(j < cols){
         row = row.append(_matrix_get_from_data(aug_data, i, j))
         j += 1
      }
      A_data = A_data.append(row)
      b_vec = b_vec.append(_matrix_get_from_data(aug_data, i, cols))
      i += 1
   }
   [_matrix_make(rows, cols, A_data), b_vec]
}

fn _matrix_require(any m, str name) any {
   if(!is_matrix(m)){ panic(name + ": expected Matrix") }
   m
}

impl list {
   fn det(list m) any { return matrix_det(_matrix_require(m, "matrix.det")) }
   fn rank(list m) int { return matrix_rank(_matrix_require(m, "matrix.rank")) }
   fn trace(list m) any { return matrix_trace(_matrix_require(m, "matrix.trace")) }
   fn transpose(list m) list { return matrix_transpose(_matrix_require(m, "matrix.transpose")) }
   fn hnf(list m) list { return matrix_hnf(_matrix_require(m, "matrix.hnf")) }
   fn snf(list m) list { return matrix_smith_form(_matrix_require(m, "matrix.snf")) }
   fn hermite_form(list m) list { return matrix_hermite_form(_matrix_require(m, "matrix.hermite_form")) }
   fn smith_form(list m) list { return matrix_smith_form(_matrix_require(m, "matrix.smith_form")) }
   fn kernel_mod(list m, any p) list { return matrix_kernel_mod(_matrix_require(m, "matrix.kernel_mod"), p) }
   fn right_kernel_mod(list m, any p) list { return matrix_right_kernel_mod(_matrix_require(m, "matrix.right_kernel_mod"), p) }
   fn left_kernel_mod(list m, any p) list { return matrix_left_kernel_mod(_matrix_require(m, "matrix.left_kernel_mod"), p) }
   fn solve_mod(list m, list b, any p) any { return matrix_solve_mod(_matrix_require(m, "matrix.solve_mod"), b, p) }
   fn change_ring(list m, any ring) list { return matrix_change_ring(_matrix_require(m, "matrix.change_ring"), ring) }
   fn hnf_transform(list m) any { return matrix_hnf_transform(_matrix_require(m, "matrix.hnf_transform")) }
   fn snf_transform(list m) any { return matrix_snf_transform(_matrix_require(m, "matrix.snf_transform")) }
}

#main {
   def m = mat_new(2, 2, 0)
   def m2 = mat_set(mat_set(m, 0, 1, 7), 1, 0, 5)
   assert(mat_get(m2, 0, 1) == 7 && mat_get(m2, 1, 0) == 5, "matrix get/set")
   assert(mat_det2(Matrix([[1, 2], [3, 4]])) == -2, "matrix det2")
   mut direct = mat4_identity()
   direct[1] = 2.0
   direct[4] = 3.0
   mat4_set(direct, 2, 3, 9.0)
   assert(direct.len == 16 && mat4_get(direct, 0, 1) == 2.0 && mat4_get(direct, 1, 0) == 3.0 && direct[11] == 9.0, "matrix mat4 flat access")
   print("✓ std.math.matrix self-test passed")
}
