;; Keywords: lattice linmod
;; Lattice routines for linear modular relation solving.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap2.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap14.pdf
module std.math.crypto.lattice.linmod(solve_linear_mod)
use std.core
use std.math.nt
use std.math.crypto.lattice.flatter

fn solve_linear_mod(list: equations, list: bounds): list {
   "Solve Ax = b mod M using lattice reduction(LLL/CVP). equations: list of [[coeffs], target, modulus]. bounds: list of upper bounds per variable. Returns solution vector."
   def direct = _linmod_bruteforce(equations, bounds)
   if(direct != nil){ return direct }
   def NR, NV = equations.len, bounds.len
   def total_dim = NR + NV
   def S = _linmod_scale(bounds, total_dim)
   def basis = _linmod_basis(equations, bounds, S, NR, NV, total_dim)
   def Y = _linmod_target(equations, bounds, S, NR, NV, total_dim)
   def reduced_basis = lll_reduce(basis, 0.75)
   _linmod_decode(babai_cvp(reduced_basis, Y), bounds, S, NR, NV)
}

fn _linmod_search_space(list: bounds, int: cap): int {
   mut prod = 1
   mut i = 0
   while(i < bounds.len){
      def b = int(bounds.get(i))
      if(b <= 0){ return 0 }
      if(prod > cap / b){ return cap + 1 }
      prod *= b
      i += 1
   }
   prod
}

fn _linmod_solution_ok(list: vars, list: equations): bool {
   mut i = 0
   while(i < equations.len){
      def eq = equations.get(i)
      def coeffs = eq.get(0)
      def target = Z(eq.get(1))
      def m = Z(eq.get(2))
      mut acc = Z(0)
      mut j = 0
      while(j < vars.len){
         def c = (j < coeffs.len) ? coeffs.get(j) : 0
         acc += Z(c) * Z(vars.get(j))
         j += 1
      }
      if(mod(acc - target, m) != Z(0)){ return false }
      i += 1
   }
   true
}

fn _linmod_bruteforce(list: equations, list: bounds, int: cap=200000): any {
   def space = _linmod_search_space(bounds, cap)
   if(space <= 0 || space > cap){ return nil }
   mut vars = []
   mut i = 0
   while(i < bounds.len){
      vars = vars.append(0)
      i += 1
   }
   while(true){
      if(_linmod_solution_ok(vars, equations)){ return clone(vars) }
      mut idx = 0
      while(idx < bounds.len){
         vars[idx] = vars[idx] + 1
         if(vars[idx] < int(bounds.get(idx))){ break }
         vars[idx] = 0
         idx += 1
      }
      if(idx >= bounds.len){ return nil }
   }
   nil
}

fn _linmod_scale(list: bounds, int: total_dim): bigint {
   "Choose a large common lattice scaling factor from variable bounds."
   mut nS = 0
   mut i = 0
   while(i < bounds.len){
      def b = bounds.get(i)
      def bl = bigint_bit_length(Z(b))
      if(bl > nS){ nS = bl }
      i += 1
   }
   bigint_lshift(Z(1), nS + total_dim + 1)
}

fn _linmod_empty_basis(int: total_dim): list {
   mut basis = list(total_dim)
   mut i = 0
   while(i < total_dim){
      basis = basis.append(vec_zero(total_dim))
      i += 1
   }
   basis
}

fn _linmod_basis(list: equations, list: bounds, any: S, int: NR, int: NV, int: total_dim): list {
   "Build the modular-linear embedding basis."
   mut basis = _linmod_empty_basis(total_dim)
   mut i = 0
   while(i < NV){
      def bound = bounds.get(i)
      def scale = S / Z(bound)
      mut row = basis.get(NR + i)
      row.set(i, scale)
      i += 1
   }
   i = 0
   while(i < NR){
      def eq = equations.get(i)
      def coeffs = eq.get(0)
      def m = eq.get(2)
      mut j = 0
      while(j < NV){
         def c = (j < coeffs.len) ? coeffs.get(j) : 0
         mut row = basis.get(i)
         row.set(j, Z(c) * S)
         j += 1
      }
      mut row_m = basis.get(i)
      row_m.set(NV + i, Z(m) * S)
      i += 1
   }
   basis
}

fn _linmod_target(list: equations, list: bounds, any: S, int: NR, int: NV, int: total_dim): list {
   "Build the closest-vector target for modular-linear recovery."
   mut Y, i = vec_zero(total_dim), 0
   while(i < NR){
      def eq = equations.get(i)
      def target = eq.get(1)
      def m = eq.get(2)
      Y.set(i, (Z(target) % Z(m)) * S)
      i += 1
   }
   i = 0
   while(i < NV){
      def bound = bounds.get(i)
      Y.set(NR + i, (Z(bound) / 2) * (S / Z(bound)))
      i += 1
   }
   Y
}

fn _linmod_decode(any: closest, list: bounds, any: S, int: NR, int: NV): list {
   "Decode variable coordinates from a closest lattice vector."
   mut solution = list(NV)
   mut i = 0
   while(i < NV){
      def bound = bounds.get(i)
      def scale = S / Z(bound)
      def val = closest.get(NR + i)
      solution = solution.append(val / scale)
      i += 1
   }
   solution
}

fn vec_zero(int: n): list {
   "Internal: Create a zero vector of length n with bigint elements."
   mut v, i = list(n), 0
   while(i < n){
      v = v.append(Z(0))
      i += 1
   }
   v
}
