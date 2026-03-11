;; Keywords: dlp discrete-log group-theory clock
;; Discrete-log routines for clock-group discrete-log problems.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap3.pdf
module std.math.crypto.dlp.clock(clock_identity, clock_add, clock_sub, clock_neg, clock_scalar_mult, clock_on_curve, clock_recover_modulus, clock_baby_step_giant_step, clock_pohlig_hellman)
use std.core
use std.math.nt

fn clock_identity(): list {
   "Identity point for the clock group."
   [Z(0), Z(1)]
}

fn _clock_point_key(list: P): str {
   bigint_to_str(P[0]) + ":" + bigint_to_str(P[1])
}

fn _clock_order_from_factors(list: factors): any {
   mut order = Z(1)
   mut i = 0
   while(i < factors.len){
      def pair = factors[i]
      order *= bigint_pow(Z(pair[0]), pair[1])
      i += 1
   }
   order
}

fn clock_add(list: P, list: Q, any: p): list {
   "Add two clock-group points modulo p."
   def pp = Z(p)
   def x1, y1 = Z(P[0]), Z(P[1])
   def x2, y2 = Z(Q[0]), Z(Q[1])
   [mod(x1 * y2 + y1 * x2, pp), mod(y1 * y2 - x1 * x2, pp)]
}

fn clock_neg(list: P, any: p): list {
   "Inverse a clock-group point modulo p."
   [mod(-Z(P[0]), p), mod(P[1], p)]
}

fn clock_sub(list: P, list: Q, any: p): list {
   "Subtract Q from P in the clock group."
   clock_add(P, clock_neg(Q, p), p)
}

fn clock_scalar_mult(any: k, list: P, any: p, any: order=nil): list {
   "Double-and-add scalar multiplication for clock-group points."
   def pp = Z(p)
   mut kk = Z(k)
   if(order != nil){ kk = mod(kk, order) }
   mut base = [mod(P[0], pp), mod(P[1], pp)]
   if(kk < Z(0)){
      kk = -kk
      base = clock_neg(base, pp)
   }
   mut acc = clock_identity()
   while(kk > Z(0)){
      if(mod(kk, Z(2)) == Z(1)){ acc = clock_add(acc, base, pp) }
      base = clock_add(base, base, pp)
      kk = bigint_div(kk, Z(2))
   }
   acc
}

fn clock_on_curve(list: P, any: p): bool {
   "Check x^2 + y^2 = 1 modulo p."
   mod(Z(P[0]) * Z(P[0]) + Z(P[1]) * Z(P[1]) - Z(1), p) == Z(0)
}

fn clock_recover_modulus(list: points): any {
   "Recover a hidden modulus from points known to satisfy x^2 + y^2 = 1 mod p."
   mut acc = Z(0)
   mut i = 0
   while(i < points.len){
      def P = points[i]
      def residue = bigint_abs(Z(P[0]) * Z(P[0]) + Z(P[1]) * Z(P[1]) - Z(1))
      acc = (acc == Z(0)) ? residue : gcd(acc, residue)
      i += 1
   }
   acc
}

fn clock_baby_step_giant_step(list: P, list: Q, any: p, any: order): any {
   "Solve Q = xP in a clock subgroup using baby-step giant-step."
   def n = Z(order)
   if(_clock_point_key(Q) == _clock_point_key(clock_identity())){ return Z(0) }
   def m = isqrt(n) + Z(1)
   mut table = dict()
   mut j = Z(0)
   mut baby = clock_identity()
   while(j < m){
      table = table.set(_clock_point_key(baby), j)
      baby = clock_add(baby, P, p)
      j += Z(1)
   }
   def giant_step = clock_scalar_mult(m, P, p, n)
   mut i = Z(0)
   mut giant = [mod(Q[0], p), mod(Q[1], p)]
   while(i < m){
      def hit = table.get(_clock_point_key(giant), nil)
      if(hit != nil){ return mod(i * m + hit, n) }
      giant = clock_sub(giant, giant_step, p)
      i += Z(1)
   }
   -1
}

fn clock_pohlig_hellman(list: P, list: Q, any: p, list: order_factors): any {
   "Solve Q = xP in a smooth-order clock group with Pohlig-Hellman.
   order_factors is a list of [prime, exponent] pairs for the point order."
   def order = _clock_order_from_factors(order_factors)
   mut remainders = list(0)
   mut moduli = list(0)
   mut i = 0
   while(i < order_factors.len){
      def pair = order_factors[i]
      def q_power = bigint_pow(Z(pair[0]), pair[1])
      def cofactor = bigint_div(order, q_power)
      def PP = clock_scalar_mult(cofactor, P, p, order)
      def QQ = clock_scalar_mult(cofactor, Q, p, order)
      def xi = clock_baby_step_giant_step(PP, QQ, p, q_power)
      if(xi == -1){ return -1 }
      remainders = remainders.append(xi)
      moduli = moduli.append(q_power)
      i += 1
   }
   def x = crt(remainders, moduli)
   (x == nil) ? -1 : mod(x, order)
}
