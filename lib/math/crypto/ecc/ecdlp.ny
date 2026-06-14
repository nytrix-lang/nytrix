;; Keywords: ecc ecdlp math crypto public-key
;; Elliptic-curve routines for elliptic-curve discrete-log attacks.
;; Reference:
;; - https://www.secg.org/sec1-v2.pdf
;; References:
;; - std.math.crypto.ecc
;; - std.math.crypto
module std.math.crypto.ecc.ecdlp(ecdlp_baby_step_giant_step, ecdlp_brute_force, ecdlp_pollard_rho, ecdlp_recover_from_linear_collision)
use std.math.nt
use std.math.crypto.ecc.ecc

fn _ec_point_eq(any P, any Q) bool {
   if P == nil || Q == nil { return P == nil && Q == nil }
   P[0] == Q[0] && P[1] == Q[1]
}

fn _ec_point_key(any P) str {
   if P == nil { return "inf" }
   bigint_to_str(P[0]) + ":" + bigint_to_str(P[1])
}

fn ecdlp_baby_step_giant_step(list P, any Q, any curve_a, any p, any n) any {
   "Baby-step giant-step for ECDLP: find x such that Q = xP."
   if Q == nil { return Z(0) }
   if _ec_point_eq(Q, P) { return Z(1) }
   def m = isqrt(Z(n)) + Z(1)
   mut table = dict()
   mut j = Z(0)
   mut cur = nil
   while j < m {
      table = table.set(_ec_point_key(cur), j)
      cur = (cur == nil) ? P : ecc_point_add(cur, P, curve_a, p)
      j += Z(1)
   }
   def mP = ecc_scalar_mult(m, P, curve_a, p, n)
   mut i = Z(0)
   mut temp = Q
   while i < m {
      def baby = table.get(_ec_point_key(temp), nil)
      if baby != nil { return mod(i * m + baby, Z(n)) }
      temp = ecc_sub(temp, mP, curve_a, p)
      i += Z(1)
   }
   nil
}

fn _ecdlp_partition(any X) any {
   if X == nil { return Z(0) }
   mod(X[0], Z(3))
}

fn _ecdlp_step(any X, any a, any b, list P, list Q, any curve_a, any p, any n) list {
   def cls = _ecdlp_partition(X)
   if cls == Z(2) { [ecc_point_add(X, Q, curve_a, p), a, mod(b + Z(1), n)] } elif cls == Z(0) {
      [ecc_point_double(X, curve_a, p), mod(a * Z(2), n), mod(b * Z(2), n)]
   } else {
      [ecc_point_add(X, P, curve_a, p), mod(a + Z(1), n), b]
   }
}

fn ecdlp_brute_force(list P, any Q, any curve_a, any p, any n) any {
   "Brute-force solve Q = xP for x in [0, n)."
   mut cur = nil
   mut x = Z(0)
   while x < Z(n) {
      if _ec_point_eq(cur, Q) { return x }
      cur = (cur == nil) ? P : ecc_point_add(cur, P, curve_a, p)
      x += Z(1)
   }
   nil
}

fn ecdlp_pollard_rho(list P, list Q, any curve_a, any p, any n, int retries=8) any {
   "Pollard-rho for ECDLP. Returns x such that Q = xP, or nil."
   mut seed = 1
   while seed <= retries {
      mut ai, bi = Z(seed), Z(seed + 1)
      mut a2i, b2i = Z(seed + 2), Z(seed + 3)
      mut Xi = ecc_point_add(ecc_scalar_mult(ai, P, curve_a, p, n), ecc_scalar_mult(bi, Q, curve_a, p, n), curve_a, p)
      mut X2i = ecc_point_add(ecc_scalar_mult(a2i, P, curve_a, p, n), ecc_scalar_mult(b2i, Q, curve_a, p, n), curve_a, p)
      mut i = Z(1)
      while i <= Z(n) + Z(2) {
         def s1 = _ecdlp_step(Xi, ai, bi, P, Q, curve_a, p, n)
         Xi, ai = s1[0], s1[1]
         bi = s1[2]
         def t1 = _ecdlp_step(X2i, a2i, b2i, P, Q, curve_a, p, n)
         def t2 = _ecdlp_step(t1[0], t1[1], t1[2], P, Q, curve_a, p, n)
         X2i, a2i = t2[0], t2[1]
         b2i = t2[2]
         if _ec_point_eq(Xi, X2i) {
            def den = mod(b2i - bi, n)
            if den != Z(0) {
               def inv = inverse_mod(den, n)
               if inv != nil && inv != Z(0) {
                  def x = mod((ai - a2i) * inv, n)
                  if _ec_point_eq(ecc_scalar_mult(x, P, curve_a, p, n), Q) { return x }
               }
            }
            i = Z(n) + Z(3)
         } else {
            i += Z(1)
         }
      }
      seed += 1
   }
   nil
}

fn ecdlp_recover_from_linear_collision(any c1, any d1, any c2, any d2, any n) any {
   "Recover x from a collision c1*P + d1*Q = c2*P + d2*Q where Q = x*P.
   Returns x = (c1-c2)/(d2-d1) mod n, or nil if the denominator is not invertible."
   def den = mod(Z(d2) - Z(d1), Z(n))
   if den == Z(0) { return nil }
   def inv = inverse_mod(den, Z(n))
   if inv == nil || inv == Z(0) { return nil }
   mod((Z(c1) - Z(c2)) * inv, Z(n))
}
