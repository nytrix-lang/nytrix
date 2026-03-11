;; Keywords: lattice linear-algebra matrix-dlog
;; Lattice routines for matrix discrete-log attacks.
;; Supports direct small finite-field cases. Jordan-form
;; extension-field lifting is intentionally not hidden here; this module exposes
;; the practical brute path.
module std.math.crypto.lattice.matrix_dlog(matrix_mod, matrix_mul_mod, matrix_pow_mod, matrix_dlog_brute)
use std.core
use std.math.matrix as matrix
use std.math.nt

fn matrix_mod(any: A, any: p): any {
   "Reduce every matrix entry modulo p."
   matrix.matrix_mod(A, p)
}

fn matrix_mul_mod(any: A, any: B, any: p): any {
   "Multiply matrices A and B over GF(p)."
   matrix.matrix_mul_mod(A, B, p)
}

fn matrix_pow_mod(any: A, any: e, any: p): any {
   "Raise matrix A to exponent e over GF(p)."
   matrix.matrix_pow_mod(A, e, p)
}

fn _matrix_eq_mod(any: A, any: B, any: p): bool {
   "Return true when matrices A and B are equal modulo p."
   matrix.matrix_eq_mod(A, B, p)
}

fn matrix_dlog_brute(any: A, any: B, any: p, any: order): any {
   "Find l such that A^l = B over GF(p), by brute force up to order."
   mut cur = matrix.matrix_identity(A[0])
   mut l = Z(0)
   while(l <= Z(order)){
      if(_matrix_eq_mod(cur, B, p)){ return l }
      cur = matrix_mul_mod(cur, A, p)
      l = l + Z(1)
   }
   nil
}
