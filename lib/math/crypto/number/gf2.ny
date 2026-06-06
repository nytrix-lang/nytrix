;; Keywords: number-theory gf2 math crypto
;; Crypto number-theory routines for GF(2) linear algebra solving.
;; Reference:
;; - https://en.wikipedia.org/wiki/Gaussian_elimination
;; References:
;; - std.math.crypto.number
;; - std.math.crypto
module std.math.crypto.number.gf2(gf2_solve, gf2_solve_full_rank)
use std.math.nt

fn _gf2_parity(any x) int { bigint_popcount(Z(x)) & 1 }

fn gf2_solve(list rows, list rhs, int ncols) any {
   "Solve rows*x = rhs over GF(2), where each row is a bigint bitmask.
   Returns one bigint solution with free variables set to zero, or nil when
   the system is inconsistent."
   if(rows.len != rhs.len || ncols <= 0){ return nil }
   mut basis = []
   mut brhs = []
   mut i = 0
   while(i < ncols){
      basis = basis.append(Z(0))
      brhs = brhs.append(0)
      i += 1
   }
   mut rank = 0
   i = 0
   while(i < rows.len){
      mut v = Z(rows[i])
      mut r = int(rhs[i]) & 1
      mut inserted = false
      while(v != Z(0)){
         def p = bit_length(v) - 1
         if(basis[p] != Z(0)){
            v = bigint_xor(v, basis[p])
            r = r ^^ brhs[p]
         } else {
            basis[p] = v
            brhs[p] = r
            rank += 1
            inserted = true
            v = Z(0)
         }
      }
      if(!inserted && v == Z(0) && r != 0){ return nil }
      i += 1
   }
   mut sol = Z(0)
   i = 0
   while(i < ncols){
      if(basis[i] != Z(0)){
         def low = basis[i] & ((Z(1) << i) - Z(1))
         def bit = brhs[i] ^^ _gf2_parity(low & sol)
         if(bit == 1){ sol += Z(1) << i }
      }
      i += 1
   }
   sol
}

fn gf2_solve_full_rank(list rows, list rhs, int ncols) any {
   "Solve rows*x = rhs over GF(2), requiring pivots for every column.
   Returns a bigint whose bit i is x_i, or nil when the system is inconsistent
   or underdetermined."
   if(rows.len != rhs.len || ncols <= 0){ return nil }
   def sol = gf2_solve(rows, rhs, ncols)
   if(sol == nil){ return nil }
   mut basis = []
   mut i = 0
   while(i < ncols){
      basis = basis.append(Z(0))
      i += 1
   }
   i = 0
   while(i < rows.len){
      mut v = Z(rows[i])
      while(v != Z(0)){
         def p = bit_length(v) - 1
         if(basis[p] != Z(0)){
            v = bigint_xor(v, basis[p])
         } else {
            basis[p] = v
            v = Z(0)
         }
      }
      i += 1
   }
   i = 0
   while(i < ncols){
      if(basis[i] == Z(0)){ return nil }
      i += 1
   }
   sol
}
