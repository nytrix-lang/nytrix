;; Keywords: lattice acd math crypto number-theory
;; Lattice routines for approximate-common-divisor lattice attacks.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap1.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap12.pdf
;; References:
;; - std.math.crypto.lattice
;; - std.math.crypto
module std.math.crypto.lattice.acd(acd_solve_sda)
use std.core
use std.math.nt
use std.math.crypto.lattice.lll

fn _acd_abs(any x) any { x < 0 ? -x : x }

fn _acd_residue_gcd(list x, int rho_bits) any {
   if rho_bits < 0 || rho_bits > 10 { return nil }
   def R = Z(1) << rho_bits
   def x0 = Z(x.get(0))
   mut best_all = Z(0)
   mut r0 = -R
   while r0 <= R {
      def base = x0 - r0
      if base != Z(0) {
         mut g = _acd_abs(base)
         mut ok = true
         mut i = 1
         while i < x.len && ok {
            mut best = Z(0)
            mut ri = -R
            while ri <= R {
               def gi = gcd(g, Z(x.get(i)) - ri)
               if gi > best { best = gi }
               ri += Z(1)
            }
            if best <= Z(1) { ok = false } else { g = best }
            i += 1
         }
         if ok && g > best_all { best_all = g }
      }
      r0 += Z(1)
   }
   best_all > Z(1) ? best_all : nil
}

fn acd_solve_sda(list x, int rho_bits) any {
   "Solve Approximate Common Divisor problem using LLL. x: samples x_i = p*q_i + r_i, rho_bits: bound on r_i. Returns recovered p or nil."
   def n = x.len
   if n < 2 { return nil }
   def direct = _acd_residue_gcd(x, rho_bits)
   if direct != nil { return direct }
   def R = bigint_lshift(Z(1), rho_bits + 1)
   mut basis = []
   mut i = 0
   while i < n {
      basis = basis.append(vec_zero(n))
      i += 1
   }
   mut row0 = basis.get(0)
   row0[0] = R
   i = 1
   while i < n {
      row0[i] = Z(x.get(i))
      mut row = basis.get(i)
      row[i] = 0 - Z(x.get(0))
      basis[i] = row
      i += 1
   }
   basis[0] = row0
   def reduced = lll(basis)
   def matrix_data = is_list(reduced.get(0, nil)) ? reduced : reduced.get(2)
   i = 0
   while i < matrix_data.len {
      def v = matrix_data.get(i)
      def v0 = v.get(0)
      if v0 != 0 && (v0 % R == 0) {
         def q0 = v0 / R
         if q0 != 0 {
            def x0, r0 = Z(x.get(0)), x0 % q0
            def p = (x0 - r0) / q0
            if p > 0 { return p }
            if p < 0 { return -p }
         }
      }
      i += 1
   }
   nil
}

fn vec_zero(int n) list {
   "Internal: Create a zero vector of length n with bigint elements."
   mut v, i = list(0), 0
   while i < n {
      v = v.append(Z(0))
      i += 1
   }
   v
}
