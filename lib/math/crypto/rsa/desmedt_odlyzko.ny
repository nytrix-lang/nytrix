;; Keywords: rsa desmedt-odlyzko math crypto
;; RSA Desmedt-Odlyzko RSA attack routines.
;; Reference:
;; - "Practical Cryptanalysis of ISO 9796-2 and EMV Signatures" (Section 3)
;; References:
;; - std.math.crypto.rsa
;; - std.math.crypto
module std.math.crypto.rsa.desmedt_odlyzko(desmedt_odlyzko_attack)
use std.core
use std.math.nt
use std.math.matrix (Matrix, matrix_solve_mod)
use std.math.crypto.rsa.op (compute_phi, compute_d)

fn _modp(any x, any p) any {
   def r = x % p
   if(r < 0){ return r + p }
   r
}

fn _prime_basis_upto(any B) list {
   mut primes = []
   mut p = Z(2)
   while(p <= B){
      primes = primes.append(p)
      p = next_prime(p)
   }
   primes
}

fn _prime_index_map(list primes) dict {
   mut out = dict(primes.len * 2 + 1)
   mut i = 0
   while(i < primes.len){
      out = out.set(bigint_to_str(Z(primes[i])), i)
      i += 1
   }
   out
}

fn _smooth_factor_vector(any v, list primes, dict prime_pos, any modp_value) any {
   def facs = factor(v)
   mut vec = list(primes.len)
   __list_set_len(vec, primes.len)
   mut i = 0
   while(i < primes.len){
      __store_item_fast(vec, i, 0)
      i += 1
   }
   i = 0
   while(i < facs.len){
      def ent = facs.get(i)
      def p = ent.get(0)
      def e = ent.get(1)
      def idx = prime_pos.get(bigint_to_str(Z(p)), -1)
      if(idx < 0){ return nil }
      __store_item_fast(vec, idx, _modp(e, modp_value))
      i += 1
   }
   vec
}

fn _rows_plus(list rows, list row) list {
   mut out = list(rows.len + 1)
   __list_set_len(out, rows.len + 1)
   mut i = 0
   while(i < rows.len){
      __store_item_fast(out, i, rows[i])
      i += 1
   }
   __store_item_fast(out, rows.len, row)
   out
}

fn _echelon_accept_row(list echelon_rows, list pivots, list vec, any p) list {
   def n = vec.len
   mut work = list(n)
   __list_set_len(work, n)
   mut i = 0
   while(i < n){
      __store_item_fast(work, i, _modp(vec[i], p))
      i += 1
   }
   i = 0
   while(i < echelon_rows.len){
      def pivot = pivots[i]
      def factor = _modp(work[pivot], p)
      if(factor != 0){
         def erow = echelon_rows[i]
         mut j = pivot
         while(j < n){
            work[j] = _modp(work[j] - factor * erow[j], p)
            j += 1
         }
      }
      i += 1
   }
   mut pivot = -1
   i = 0
   while(i < n && pivot < 0){
      if(_modp(work[i], p) != 0){ pivot = i }
      i += 1
   }
   if(pivot < 0){ return [false, echelon_rows, pivots] }
   def inv = inverse_mod(work[pivot], p)
   i = pivot
   while(i < n){
      work[i] = _modp(work[i] * inv, p)
      i += 1
   }
   [true, _rows_plus(echelon_rows, work), pivots.append(pivot)]
}

fn _solve_coefficients(list rows, list target, any p) any {
   def l = target.len
   if(rows.len != l){ return nil }
   mut A = list(l)
   __list_set_len(A, l)
   mut eq = 0
   while(eq < l){
      mut row = list(l)
      __list_set_len(row, l)
      mut vi = 0
      while(vi < l){
         __store_item_fast(row, vi, _modp(rows[vi][eq], p))
         vi += 1
      }
      __store_item_fast(A, eq, row)
      eq += 1
   }
   matrix_solve_mod(Matrix(A), target, p)
}

fn desmedt_odlyzko_attack(fnptr hash_oracle, fnptr sign_oracle, any N, any e, any target_m) any {
   "Selective forgery attack using a smooth-hash basis and modular linear algebra.
   Returns a signature integer for target_m, or nil if a basis cannot be built."
   if(!is_prime(e)){ return nil }
   def target_hash = hash_oracle(target_m)
   def target_factors = factor(target_hash)
   if(target_factors.len == 0){ return nil }
   def B = target_factors.get(target_factors.len - 1).get(0)
   def primes = _prime_basis_upto(B)
   def l = primes.len
   if(l == 0){ return nil }
   def prime_pos = _prime_index_map(primes)
   def target_vec = _smooth_factor_vector(target_hash, primes, prime_pos, e)
   if(target_vec == nil){ return nil }
   mut basis_rows = []
   mut basis_msgs = []
   mut echelon_rows = []
   mut pivots = []
   mut mi = 0
   while(basis_rows.len < l){
      def h = hash_oracle(mi)
      def vec = _smooth_factor_vector(h, primes, prime_pos, e)
      if(vec != nil){
         def accepted = _echelon_accept_row(echelon_rows, pivots, vec, e)
         if(accepted[0]){
            echelon_rows = accepted[1]
            pivots = accepted[2]
            basis_rows = basis_rows.append(vec)
            basis_msgs = basis_msgs.append(mi)
         }
      }
      mi += 1
      if(mi > 100000){ return nil }
   }
   def coeffs = _solve_coefficients(basis_rows, target_vec, e)
   if(coeffs == nil){ return nil }
   mut sig = Z(1)
   mut i = 0
   while(i < coeffs.len){
      def c = coeffs.get(i)
      if(c != 0){
         def si = sign_oracle(basis_msgs.get(i))
         sig = (sig * power_mod(si, c, N)) % N
      }
      i += 1
   }
   sig
}
