;; Keywords: dlp discrete-log group-theory math crypto
;; Discrete-log routines for generic discrete-log solvers and Pohlig-Hellman composition.
;; baby_step_giant_step: O(sqrt(order)) time and space.
;; pohlig_hellman: reduces DLP to subgroup DLPs, efficient when order is smooth.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap3.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap5.pdf
;; References:
;; - std.math.crypto.dlp
;; - std.math.crypto
module std.math.crypto.dlp.dlp(multiplicative_order_from_factors, multiplicative_order_factorization, baby_step_giant_step, pohlig_hellman, pohlig_hellman_bounded, ph_prime_power, ph_recombine, pohlig_hellman_prime_power, pohlig_hellman_recombine, pollard_rho_dlp, dlp_brute_force, solve_dlp)
use std.core
use std.core.dict_mod
use std.math.nt

fn baby_step_giant_step(any g, any h, any p, any order=nil) any {
   "Solve the discrete log g^x = h(mod p) using baby-step giant-step.
   order: group order(defaults to p-1 for Fp*).
   Returns x if found, nil otherwise.
   Time and space: O(sqrt(order))."
   if order == nil { order = Z(p) - Z(1) } else { order = Z(order) }
   def gg, pp = Z(g), Z(p)
   def m = isqrt(order) + Z(1)
   mut table = dict()
   mut i = Z(0)
   mut curr = Z(1)
   while i < m {
      table = dict_write(table, bigint_to_str(curr), i + Z(1))
      curr = mod(curr * gg, pp)
      i = i + Z(1)
   }
   def gm_inv = inverse_mod(power_mod(gg, m, pp), pp)
   mut j = Z(0)
   mut gamma = mod(h, pp)
   while j < m {
      def key = bigint_to_str(gamma)
      if dict_exists(table, key) {
         def x = j * m + dict_read(table, key) - Z(1)
         return mod(x, order)
      }
      gamma = mod(gamma * gm_inv, pp)
      j = j + Z(1)
   }
   -1
}

fn pohlig_hellman(any g, any h, any p, list order_factors) any {
   "Solve g^x = h(mod p) via Pohlig-Hellman decomposition.
   order_factors: list of [prime, exponent] pairs whose product equals the group order.
   Decomposes the DLP into DLPs in prime-power order subgroups, then recombines via CRT.
   Returns x in [0, order) or nil if any subproblem fails."
   mut rems = list(0)
   mut mods = list(0)
   def order = _ph_order_from_factors(order_factors)
   mut fi = 0
   while fi < order_factors.len {
      def pair = order_factors[fi]
      def qi = pair[0]
      def ei = pair[1]
      def qi_e = _ph_pow(qi, ei)
      def cofactor = bigint_div(order, qi_e)
      def h_i = power_mod(h, cofactor, p)
      def g_i = power_mod(g, cofactor, p)
      def x_i = _ph_solve_prime_power(g_i, h_i, p, qi, ei)
      if x_i == -1 { return -1 }
      rems = rems.append(x_i)
      mods = mods.append(qi_e)
      fi += 1
   }
   def x = crt(rems, mods)
   (x == nil) ? -1 : x
}

fn pohlig_hellman_bounded(any g, any h, any p, list order_factors, any exponent_bound) any {
   "Solve g^x = h(mod p) via enough Pohlig-Hellman factors to identify x < exponent_bound.
   Useful when the discrete log encodes a bounded plaintext or nonce ; selected residues are recombined
   with CRT, and callers can verify the full group equation afterward."
   mut rems = list(0)
   mut mods = list(0)
   def order = _ph_order_from_factors(order_factors)
   mut modulus = Z(1)
   def bound = Z(exponent_bound)
   mut i = 0
   while i < order_factors.len && modulus <= bound {
      def pair = order_factors[i]
      def qi = pair[0]
      def ei = pair[1]
      def qi_e = _ph_pow(qi, ei)
      def cofactor = bigint_div(order, qi_e)
      def h_i = power_mod(h, cofactor, p)
      def g_i = power_mod(g, cofactor, p)
      def x_i = _ph_solve_prime_power(g_i, h_i, p, qi, ei)
      if x_i == -1 { return -1 }
      rems = rems.append(x_i)
      mods = mods.append(qi_e)
      modulus = modulus * qi_e
      i += 1
   }
   if modulus <= bound { return -1 }
   def x = crt(rems, mods)
   (x == nil) ? -1 : x
}

fn pohlig_hellman_prime_power(any g, any h, any p, any q, any e) any {
   "Solve g^x = h(mod p) in a subgroup of order q^e.
   Returns x in [0, q^e) or -1."
   _ph_solve_prime_power(g, h, p, q, e)
}

fn pohlig_hellman_recombine(list remainders, list moduli) any {
   "CRT recombination helper for Pohlig-Hellman residues."
   def x = crt(remainders, moduli)
   (x == nil) ? -1 : x
}

fn ph_prime_power(any g, any h, any p, any q, any e) any {
   "Short export wrapper for prime-power Pohlig-Hellman."
   pohlig_hellman_prime_power(g, h, p, q, e)
}

fn ph_recombine(list remainders, list moduli) any {
   "Short export wrapper for CRT recombination."
   pohlig_hellman_recombine(remainders, moduli)
}

fn _ph_order_from_factors(list factors) any {
   mut ord = Z(1)
   mut i = 0
   while i < factors.len {
      def pair = factors[i]
      ord = ord * _ph_pow(pair[0], pair[1])
      i += 1
   }
   ord
}

fn multiplicative_order_from_factors(any g, any p, list group_order_factors) any {
   "Compute the multiplicative order of g modulo p from a factorization of the ambient group order."
   def pp = Z(p)
   def gg = Z(g) % pp
   mut order = _ph_order_from_factors(group_order_factors)
   mut i = 0
   while i < group_order_factors.len {
      def q = Z(group_order_factors[i][0])
      while order % q == Z(0) && power_mod(gg, order / q, pp) == Z(1) {
         order = order / q
      }
      i += 1
   }
   order
}

fn multiplicative_order_factorization(any g, any p, list group_order_factors) list {
   "Return the factorization of ord_p(g) from a factorization of p-1."
   def order = multiplicative_order_from_factors(g, p, group_order_factors)
   mut out = []
   mut i = 0
   while i < group_order_factors.len {
      def q = Z(group_order_factors[i][0])
      mut exp = 0
      mut t = order
      while t % q == Z(0) {
         exp += 1
         t = t / q
      }
      if exp > 0 { out = out.append([q, exp]) }
      i += 1
   }
   out
}

fn _ph_pow(any base, any exp) any {
   mut result = Z(1)
   mut b = Z(base)
   mut e = Z(exp)
   while e > Z(0) {
      if mod(e, Z(2)) == Z(1) { result = result * b }
      b, e = b * b, bigint_div(e, Z(2))
   }
   result
}

fn _ph_solve_prime_power(any g, any h, any p, any q, any e) any {
   "Solve g^x = h(mod p) where g has order q^e.
   Uses iterative lifting: recover x mod q, then x mod q^2, etc.
   Returns x in [0, q^e) or nil."
   def ee = int(e)
   def q_e = _ph_pow(q, e)
   def g_q = power_mod(g, _ph_pow(q, ee - 1), p)
   mut x = Z(0)
   mut layer = 0
   while layer < ee {
      def g_inv_x = power_mod(inverse_mod(g, p), x, p)
      def hx = mod(Z(h) * g_inv_x, p)
      def exp_layer = _ph_pow(q, ee - 1 - layer)
      def h_layer = power_mod(hx, exp_layer, p)
      def d_layer = _ph_solve_digit(g_q, h_layer, p, q)
      if d_layer == -1 { return -1 }
      x = x + d_layer * _ph_pow(q, layer)
      layer += 1
   }
   mod(x, q_e)
}

fn _ph_brute_q(any g, any h, any p, any q) any {
   def gg, pp, qq = Z(g), Z(p), Z(q)
   def hh = mod(h, pp)
   mut curr = Z(1)
   mut i = Z(0)
   while i < qq {
      if curr == hh { return i }
      curr = mod(curr * gg, pp)
      i = i + Z(1)
   }
   -1
}

fn _ph_solve_digit(any g, any h, any p, any q) any {
   "Solve a Pohlig-Hellman digit in a subgroup of prime order q."
   if Z(q) <= Z(4096) { return _ph_brute_q(g, h, p, q) }
   def x = baby_step_giant_step(g, h, p, q)
   x == nil ? -1 : x
}

fn _rho_state_next(any xi, any ai, any bi, any g, any h, any p, any n) list {
   def cls = mod(xi, Z(3))
   if cls == Z(2) { [mod(xi * h, p), ai, mod(bi + Z(1), n)] } elif cls == Z(0) {
      [power_mod(xi, Z(2), p), mod(ai * Z(2), n), mod(bi * Z(2), n)]
   } else {
      [mod(xi * g, p), mod(ai + Z(1), n), bi]
   }
}

fn pollard_rho_dlp(any g, any h, any p, any n) any {
   "Pollard-rho discrete log in a prime-order subgroup.
   Solves g^x = h(mod p). Returns x or nil."
   if n == nil { return -1 }
   def gg, hh, pp, nn = Z(g), Z(h), Z(p), Z(n)
   if nn <= Z(1) { return -1 }
   mut xi = Z(1)
   mut x2i = Z(1)
   mut ai = Z(0)
   mut bi = Z(0)
   mut a2i = Z(0)
   mut b2i = Z(0)
   mut i = Z(1)
   while i <= nn + Z(2) {
      def s1 = _rho_state_next(xi, ai, bi, gg, hh, pp, nn)
      xi, ai = s1[0], s1[1]
      bi = s1[2]
      def t1, t2 = _rho_state_next(x2i, a2i, b2i, gg, hh, pp, nn), _rho_state_next(t1[0], t1[1], t1[2], gg, hh, pp, nn)
      x2i, a2i = t2[0], t2[1]
      b2i = t2[2]
      if xi == x2i {
         def r = mod(bi - b2i, nn)
         if r == Z(0) { return -1 }
         def rinv = inverse_mod(r, nn)
         if rinv == nil || rinv == Z(0) { return -1 }
         return mod(rinv * (a2i - ai), nn)
      }
      i = i + Z(1)
   }
   -1
}

fn dlp_brute_force(any g, any h, any p, any order=nil) any {
   "Brute-force solve g^x = h(mod p). Iterates x from 0 to order.
   Returns x if found, nil otherwise."
   if order == nil { order = Z(p) - Z(1) } else { order = Z(order) }
   def gg, pp = Z(g), Z(p)
   def hh = mod(h, pp)
   mut curr = Z(1)
   mut x = Z(0)
   while x <= order {
      if curr == hh { return x }
      curr = mod(curr * gg, pp)
      x = x + Z(1)
   }
   -1
}

fn solve_dlp(any g, any h, any p, any order=nil) list {
   "Solve the discrete log g^x = h(mod p) by trying BSGS, Pollard-rho, then brute force.
   Returns [x, method] if found, nil otherwise."
   def x = baby_step_giant_step(g, h, p, order)
   if x != -1 { return [x, "bsgs"] }
   if order != nil {
      def xr = pollard_rho_dlp(g, h, p, order)
      if xr != -1 { return [xr, "pollard_rho"] }
   }
   def x2 = dlp_brute_force(g, h, p, order)
   if x2 != -1 { return [x2, "brute"] }
   [-1, "none"]
}

#main {
   def p, g = Z(23), Z(5)
   def h1 = power_mod(g, Z(6), p)
   def brute = dlp_brute_force(g, h1, p, Z(22))
   assert(brute == Z(6), "brute-force exact solution")
   assert(power_mod(g, brute, p) == h1, "brute-force verifies")
   def bsgs = baby_step_giant_step(g, h1, p, Z(22))
   assert(bsgs == Z(6), "bsgs exact solution")
   assert(power_mod(g, bsgs, p) == h1, "bsgs verifies")
   def solved = solve_dlp(g, h1, p, Z(22))
   assert(solved != nil, "solve_dlp returns result")
   assert(solved[0] == Z(6), "solve_dlp exact solution")
   assert(solved[1] == "bsgs", "solve_dlp method")
   def h2 = power_mod(Z(5), Z(9), Z(23))
   def ph = pohlig_hellman(Z(5), h2, Z(23), [[2, 1], [11, 1]])
   assert(ph == Z(9), "pohlig-hellman exact solution")
   assert(power_mod(Z(5), ph, Z(23)) == h2, "pohlig-hellman verifies")
   assert(multiplicative_order_from_factors(Z(2), Z(23), [[2, 1], [11, 1]]) == Z(11), "multiplicative order from factors")
   assert(multiplicative_order_factorization(Z(2), Z(23), [[2, 1], [11, 1]]) == [[Z(11), 1]], "multiplicative order factorization")
   def p5, g5 = 5, 2
   def h5 = power_mod(g5, 3, p5)
   def ph_pp = pohlig_hellman(g5, h5, p5, [[2, 2]])
   assert(ph_pp == 3, "pohlig-hellman prime power")
   assert(power_mod(g5, ph_pp, p5) == h5, "pohlig-hellman prime power verifies")
   def pp = pohlig_hellman_prime_power(g5, h5, p5, 2, 2)
   assert(pp == 3, "pohlig_hellman_prime_power exact")
   def p_big, g_big = Z(10007), Z(25)
   def h_big = power_mod(g_big, Z(1234), p_big)
   def x_big = pohlig_hellman(g_big, h_big, p_big, [[5003, 1]])
   assert(x_big == Z(1234), "pohlig-hellman bsgs digit")
   assert(pohlig_hellman_recombine([1, 2], [3, 5]) == 7, "pohlig_hellman_recombine")
   def rho_h = power_mod(Z(2), Z(77), Z(383))
   def rho = pollard_rho_dlp(Z(2), rho_h, Z(383), Z(191))
   assert(rho == Z(77), "pollard rho exact solution")
   assert(power_mod(Z(2), rho, Z(383)) == rho_h, "pollard rho verifies")
   print("✓ std.math.crypto.dlp.dlp self-test passed")
}
