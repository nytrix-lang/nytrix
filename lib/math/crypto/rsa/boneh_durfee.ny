;; Keywords: rsa boneh-durfee
;; RSA Boneh-Durfee small-private-exponent attack routines.
;; Reference:
;; - https://crypto.stanford.edu/~dabo/pubs/abstracts/lowRSAexp.html
;; Uses LLL and bivariate Coppersmith-style resultant elimination.
module std.math.crypto.rsa.boneh_durfee(boneh_durfee_attack)
use std.core
use std.math.nt
use std.math (pow)
use std.math.matrix (Matrix, _matrix_cols, _matrix_data, _matrix_get, _matrix_rows, _matrix_set)
use std.math.crypto.lattice.lll
use std.math.crypto.lattice.coppersmith
use std.math.crypto.poly

fn boneh_durfee_attack(any: e, any: n, f64: delta=0.292, int: m=3, any: t=nil): any {
   "Boneh-Durfee attack on RSA with small private exponent d < n^0.292.
   Uses LLL on a bivariate lattice to find small roots of f(x, y) = x(A + y) + 1 mod e."
   if(e <= 1 || n <= 1 || m < 1){ return nil }
   if(!t){ t = int(float(m) * (1.0 - 2.0*delta) / float(delta)) }
   def A, X = bigint_div(bigint_add(n, bigint(1)), bigint(2)), bigint_from_int(int(pow(float(n), delta)))
   def Y = bigint_from_int(int(pow(float(n), 0.5)))
   mut shifts = []
   mut j = 0 while(j <= m){
      mut i = 0 while(i <= m - j){
         shifts = shifts.append(_bd_gen_shift(i, j, m - j, A, e, X, Y))
         i += 1
      }
      j += 1
   }
   j = 0 while(j <= m){
      mut i = 0 while(i <= t){
         shifts = shifts.append(_bd_gen_shift(i, j, 0, A, bigint(1), X, Y))
         i += 1
      }
      j += 1
   }
   def B = Matrix(shifts)
   def reduced = lll(B)
   def res = _bd_extract_roots(reduced, X, Y, m)
   if(res == nil || res.len == 0){ return nil }
   def y0 = res.get(0)
   def phi = n + 1 + y0
   def d = inverse_mod(e, phi)
   [d, phi]
}

fn _bd_gen_shift(int: i, int: j, int: k, any: A, any: e, any: X, any: Y): list {
   mut res = poly2_new(i + j + 1, j + 1)
   _poly2_to_flat_vector(res, X, Y)
}

fn _bd_extract_roots(any: basis, any: X, any: Y, int: m): any {
   def row1, row2 = _matrix_data(basis).get(0), _matrix_data(basis).get(1)
   def p1, p2 = _flat_vector_to_poly2(row1, X, Y, m, m), _flat_vector_to_poly2(row2, X, Y, m, m)
   def res_y = poly2_resultant_x(p1, p2)
   poly_small_roots(res_y, Y)
}

fn _poly2_to_flat_vector(any: p, any: X, any: Y): list {
   def r, c = _matrix_rows(p), _matrix_cols(p)
   mut vec = []
   mut i = 0 while(i < r){
      mut j = 0 while(j < c){
         def v = _matrix_get(p, i, j)
         vec = vec.append(bigint_mul(v, bigint_mul(bigint_pow(X, Z(i)), bigint_pow(Y, Z(j)))))
         j += 1
      }
      i += 1
   }
   vec
}

fn _flat_vector_to_poly2(list: vec, any: X, any: Y, int: r, int: c): any {
   mut p = poly2_new(r, c)
   mut idx = 0
   mut i = 0 while(i < r){
      mut j = 0 while(j < c){
         def val = vec.get(idx)
         def coeff = bigint_div(val, bigint_mul(bigint_pow(X, Z(i)), bigint_pow(Y, Z(j))))
         _matrix_set(p, i, j, coeff)
         idx += 1 j += 1
      }
      i += 1
   }
   p
}
