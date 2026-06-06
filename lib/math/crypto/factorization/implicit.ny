;; Keywords: factorization implicit math crypto number-theory
;; Integer-factorization routines for implicit factorization from related moduli.
;; Reference:
;; - Nitaj A., Ariffin M.R.K., "Implicit factorization of unbalanced RSA moduli"
;; References:
;; - std.math.crypto.factorization
;; - std.math.crypto
module std.math.crypto.factorization.implicit(implicit_factorize_msb, implicit_factorize_lsb)
use std.math.nt
use std.math.crypto.lattice.flatter

fn _abs(any x) any { x < 0 ? (0 - x) : x }

fn _recover_factors(list v, list mods) any {
   mut out = []
   mut i = 0
   while(i < mods.len && i < v.len){
      def ni, qi = mods.get(i), gcd(_abs(v.get(i)), ni)
      if(qi <= 1 || qi >= ni || (ni % qi) != 0){ return nil }
      out = out.append([ni / qi, qi])
      i += 1
   }
   out.len == mods.len ? out : nil
}

fn _trial_factor_pair(any n, int bound=10000) any {
   def nn = _abs(n)
   if(nn < 4){ return nil }
   if(nn % 2 == 0){ return [2, nn / 2] }
   mut p = 3
   while(p <= bound && p * p <= nn){
      if(nn % p == 0){ return [p, nn / p] }
      p += 2
   }
   nil
}

fn _trial_factor_mods(list mods) any {
   mut out = []
   mut i = 0
   while(i < mods.len){
      def pair = _trial_factor_pair(mods.get(i))
      if(pair == nil){ return nil }
      out = out.append(pair)
      i += 1
   }
   out
}

fn _basis_candidates(list basis) list {
   mut out = []
   def reduced = lll_reduce(basis, 0.75)
   if(reduced.len == 0){ return out }
   mut best_i = 0
   mut best_norm = vec_norm_sq(reduced.get(0))
   mut i = 1
   while(i < reduced.len){
      def norm = vec_norm_sq(reduced.get(i))
      if(norm < best_norm){
         best_i = i
         best_norm = norm
      }
      i += 1
   }
   out = out.append(reduced.get(best_i))
   i = 0
   while(i < reduced.len){
      if(i != best_i){ out = out.append(reduced.get(i)) }
      i += 1
   }
   out
}

fn implicit_factorize_msb(list mods, int n, int t) any {
   "Factor moduli when the hidden cofactors share most-significant bits.
   mods: list of RSA moduli, n: modulus bit length, t: shared MSB count.
   Returns list of [p, q] factor pairs or nil."
   if(mods.len < 2){ return nil }
   mut basis = []
   mut row0 = []
   row0 = row0.append(1 << (n - t))
   mut i = 1
   while(i < mods.len){
      row0 = row0.append(mods.get(i))
      i += 1
   }
   basis = basis.append(row0)
   i = 1
   while(i < mods.len){
      mut row = []
      mut j = 0
      while(j < mods.len){
         row = row.append(j == i ? (0 - mods.get(0)) : 0)
         j += 1
      }
      basis = basis.append(row)
      i += 1
   }
   def cands = _basis_candidates(basis)
   i = 0
   while(i < cands.len){
      def r = _recover_factors(cands.get(i), mods)
      if(r != nil){ return r }
      i += 1
   }
   _trial_factor_mods(mods)
}

fn implicit_factorize_lsb(list mods, int n, int t) any {
   "Factor moduli when the hidden cofactors share least-significant bits.
   mods: list of RSA moduli, n: modulus bit length, t: shared LSB count.
   Returns list of [p, q] factor pairs or nil."
   if(mods.len < 2){ return nil }
   def two_t = 1 << t
   def n0_inv = inverse_mod(mods.get(0), two_t)
   if(n0_inv <= 0){ return nil }
   mut basis = []
   mut row0 = []
   row0 = row0.append(1)
   mut i = 1
   while(i < mods.len){
      row0 = row0.append(mod(mods.get(i) * n0_inv, two_t))
      i += 1
   }
   basis = basis.append(row0)
   i = 1
   while(i < mods.len){
      mut row = []
      mut j = 0
      while(j < mods.len){
         row = row.append(j == i ? (0 - two_t) : 0)
         j += 1
      }
      basis = basis.append(row)
      i += 1
   }
   def cands = _basis_candidates(basis)
   i = 0
   while(i < cands.len){
      def r = _recover_factors(cands.get(i), mods)
      if(r != nil){ return r }
      i += 1
   }
   _trial_factor_mods(mods)
}
