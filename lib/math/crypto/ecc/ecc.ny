;; Keywords: ecc
;; Elliptic-curve routines for elliptic-curve point arithmetic and curve operations.
;; Reference:
;; - https://www.secg.org/sec1-v2.pdf
;; Public operations favor correctness and simple affine behavior.
;; Jacobian helpers remain exported for callers that need them.
module std.math.crypto.ecc.ecc(ecc_point_add, ecc_point_double, ecc_scalar_mult, ecc_scalar_mult_jacobian,
   ecc_negate, ecc_sub, ecc_is_on_curve, ecc_parameter_recovery,
   ecc_curve_secp256k1, ecc_curve_p256, ecc_curve_p384, ecc_sqrt_mod,
   ecc_to_jacobian, ecc_from_jacobian, ecc_jacobian_add, ecc_jacobian_double,
ecc_precompute_table, ecc_glv_decompose)

use std.math.nt

fn ecc_point_add(any: P, any: Q, any: a, any: p): any {
   "Add points P and Q in Affine coordinates."
   def Pj, Qj = ecc_to_jacobian(P, p), ecc_to_jacobian(Q, p)
   ecc_from_jacobian(ecc_jacobian_add(Pj, Qj, a, p), p)
}

fn ecc_point_double(any: P, any: a, any: p): any {
   "Double point P in Affine coordinates."
   ecc_from_jacobian(ecc_jacobian_double(ecc_to_jacobian(P, p), a, p), p)
}

fn ecc_scalar_mult(any: k, any: P, any: a, any: p, any: n=nil): any {
   "Scalar multiplication k*P using Jacobian binary double-and-add."
   if(k == 0 || P == nil){ return nil }
   ecc_from_jacobian(_ecc_scalar_mult_jacobian(k, ecc_to_jacobian(P, p), a, p), p)
}

fn ecc_scalar_mult_jacobian(any: k, any: P, any: a, any: p): list {
   "Scalar multiplication k*P in Jacobian space. Returns [X,Y,Z] (Jacobian)."
   if(k == 0 || P == nil){ return [Z(0), Z(1), Z(0)] }
   _ecc_scalar_mult_jacobian(k, P, a, p)
}

fn _ecc_scalar_mult_jacobian(any: k, list: Pj, any: a, any: p): list {
   mut kk = Z(k)
   mut Pt = Pj
   if(bigint_lt(kk, Z(0))){
      kk = bigint_neg(kk)
      Pt = [Pt[0], mod_sub(0, Pt[1], p), Pt[2]]
   }
   mut R = [Z(0), Z(1), Z(0)]
   def bits = bigint_bit_length(kk)
   mut b = bits - 1
   while(b >= 0){
      R = ecc_jacobian_double(R, a, p)
      if(bigint_mod(bigint_div(kk, bigint_lshift(Z(1), b)), Z(2)) != Z(0)){ R = ecc_jacobian_add(R, Pt, a, p) }
      b -= 1
   }
   R
}

fn ecc_to_jacobian(any: P, any: p): any {
   "Convert affine point P=[x,y] into Jacobian [X,Y,Z] over Fp."
   if(P == nil){ return nil }
   [mod(P[0], p), mod(P[1], p), Z(1)]
}

fn ecc_from_jacobian(any: P, any: p): any {
   "Convert Jacobian point [X,Y,Z] back to affine coordinates over Fp."
   if(P == nil){ return nil }
   def X = P[0] def Y = P[1] def Z = P[2]
   if(mod(Z, p) == 0){ return nil }
   def Zi = inverse_mod(Z, p)
   def Zi2 = mod_mul(Zi, Zi, p)
   def Zi3 = mod_mul(Zi2, Zi, p)
   [mod_mul(X, Zi2, p), mod_mul(Y, Zi3, p)]
}

fn ecc_jacobian_double(any: P, any: a, any: p): list {
   "Double a Jacobian point on y^2 = x^3 + ax + b over Fp."
   if(P == nil || P[2] == 0 || P[1] == 0){ return [Z(0), Z(1), Z(0)] }
   def X1 = P[0] def Y1 = P[1] def Z1 = P[2]
   def Y1sq = mod_mul(Y1, Y1, p)
   def S = mod_mul(4, mod_mul(X1, Y1sq, p), p)
   mut M = mod_mul(3, mod_mul(X1, X1, p), p)
   if(a != 0){
      def Z1sq = mod_mul(Z1, Z1, p)
      M = mod_add(M, mod_mul(a, mod_mul(Z1sq, Z1sq, p), p), p)
   }
   def X3 = mod_sub(mod_mul(M, M, p), mod_mul(2, S, p), p)
   def Y3 = mod_sub(mod_mul(M, mod_sub(S, X3, p), p), mod_mul(8, mod_mul(Y1sq, Y1sq, p), p), p)
   def Z3 = mod_mul(2, mod_mul(Y1, Z1, p), p)
   [X3, Y3, Z3]
}

fn ecc_jacobian_add(any: P, any: Q, any: a, any: p): any {
   "Add two Jacobian points on y^2 = x^3 + ax + b over Fp."
   if(P == nil){ return Q }
   if(Q == nil){ return P }
   def X1 = P[0] def Y1 = P[1] def Z1 = P[2]
   def X2 = Q[0] def Y2 = Q[1] def Z2 = Q[2]
   if(Z1 == 0){ return Q }
   if(Z2 == 0){ return P }
   def Z1sq, Z2sq = mod_mul(Z1, Z1, p), mod_mul(Z2, Z2, p)
   def U1, U2 = mod_mul(X1, Z2sq, p), mod_mul(X2, Z1sq, p)
   def S1, S2 = mod_mul(mod_mul(Y1, Z2, p), Z2sq, p), mod_mul(mod_mul(Y2, Z1, p), Z1sq, p)
   if(U1 == U2){
      if(S1 != S2){ return nil }
      return ecc_jacobian_double(P, a, p)
   }
   def H, r = mod_sub(U2, U1, p), mod_sub(S2, S1, p)
   def Hsq = mod_mul(H, H, p)
   def Hcub = mod_mul(H, Hsq, p)
   def U1Hsq = mod_mul(U1, Hsq, p)
   def X3 = mod_sub(mod_sub(mod_mul(r, r, p), Hcub, p), mod_mul(2, U1Hsq, p), p)
   def Y3 = mod_sub(mod_mul(r, mod_sub(U1Hsq, X3, p), p), mod_mul(S1, Hcub, p), p)
   def Z3 = mod_mul(mod_mul(H, Z1, p), Z2, p)
   [X3, Y3, Z3]
}

fn ecc_negate(any: P, any: p): any {
   "Return the additive inverse of affine point P over Fp."
   if(P == nil){ return nil }
   [P[0], mod_sub(0, P[1], p)]
}

fn ecc_sub(any: P, any: Q, any: a, any: p): any {
   "Subtract affine point Q from P over Fp."
   ecc_point_add(P, ecc_negate(Q, p), a, p)
}

fn ecc_is_on_curve(any: P, any: a, any: b, any: p): bool {
   "Return true when affine point P lies on y^2 = x^3 + ax + b over Fp."
   if(P == nil){ return true }
   def x = P[0] def y = P[1]
   def lhs, rhs = mod_mul(y, y, p), mod_add(mod_add(bigint_pow(Z(x), Z(3)), mod_mul(a, x, p), p), b, p)
   lhs == rhs
}

fn ecc_parameter_recovery(list: pts, any: p): any {
   "Recover(a, b) from points on curve. Needs 2 points."
   if(pts.len < 2){ return nil }
   def P1 = pts[0] def P2 = pts[1]
   def x1 = P1[0] def y1 = P1[1]
   def x2 = P2[0] def y2 = P2[1]
   def num = mod_sub(mod_sub(mod_mul(y1, y1, p), bigint_pow(Z(x1), Z(3)), p),
   mod_sub(mod_mul(y2, y2, p), bigint_pow(Z(x2), Z(3)), p), p)
   def den = mod_sub(x1, x2, p)
   def a = mod_mul(num, inverse_mod(den, p), p)
   def b = mod_sub(mod_sub(mod_mul(y1, y1, p), bigint_pow(Z(x1), Z(3)), p), mod_mul(a, x1, p), p)
   [a, b]
}

fn ecc_curve_secp256k1(): list {
   "Return secp256k1 parameters as [p, a, b, G, n]."
   def p, a = hex_to_bigint("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F"), Z(0)
   def b, n = Z(7), hex_to_bigint("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141")
   def G = [hex_to_bigint("79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798"),
   hex_to_bigint("483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8")]
   [p, a, b, G, n]
}

fn ecc_curve_p256(): list {
   "Return NIST P-256 parameters as [p, a, b, G, n]."
   def p, a = hex_to_bigint("FFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF"), Z("-3")
   def b = hex_to_bigint("5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B")
   def n = hex_to_bigint("FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551")
   def G = [hex_to_bigint("6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296"),
   hex_to_bigint("4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5")]
   [p, a, b, G, n]
}

fn ecc_curve_p384(): list {
   "Return NIST P-384 parameters as [p, a, b, G, n]."
   def p = hex_to_bigint(
      "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFE" +
      "FFFFFFFF0000000000000000FFFFFFFF"
   )
   def a, b = Z("-3"), hex_to_bigint(
      "B3312FA7E23EE7E4988E056BE3F82D19181D9C6EFE8141120314088F5013875A" +
      "C656398D8A2ED19D2A85C8EDD3EC2AEF"
   )
   def n = hex_to_bigint(
      "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFC7634D81F4372DDF" +
      "581A0DB248B0A77AECEC196ACCC52973"
   )
   def G = [
      hex_to_bigint(
         "AA87CA22BE8B05378EB1C71EF320AD746E1D3B628BA79B9859F741E082542A38" +
         "5502F25DBF55296C3A545E3872760AB7"
      ),
      hex_to_bigint(
         "3617DE4A96262C6F5D9E98BF9292DC29F8F41DBD289A147CE9DA3113B5F0B8C0" +
         "0A60B1CE1D7E819D7A431D7C90EA0E5F"
      )
   ]
   [p, a, b, G, n]
}

fn ecc_sqrt_mod(any: n, any: p): any {
   "Return a square root of n modulo prime p using Tonelli-Shanks."
   tonelli_shanks(n, p)
}

fn ecc_precompute_table(any: P, any: a, any: p, int: w=4): list {
   "Build a small affine precomputation table for windowed scalar multiplication."
   mut table = [P]
   mut i = 1
   while(i < (1 << (w - 1))){
      table = table.append(ecc_point_add(table[i - 1], P, a, p))
      i += 1
   }
   table
}

fn ecc_glv_decompose(any: k, any: n): list {
   "Decompose a secp256k1 scalar into GLV basis components."
   def b11 = hex_to_bigint("3086D221A7D46BCDE86C90E49284EB15")
   def b12 = bigint_neg(hex_to_bigint("E4437ED6010E88286F547FA90ABFE4C3"))
   def b21 = hex_to_bigint("114CA50F7A8E2F3F657C1108D9D44CFB8")
   def b22 = hex_to_bigint("3086D221A7D46BCDE86C90E49284EB15")
   def c1 = bigint_div(bigint_mul(k, b22), n)
   def c2 = bigint_div(bigint_mul(k, bigint_neg(b12)), n)
   mut k1 = bigint_sub(k, bigint_add(bigint_mul(c1, b11), bigint_mul(c2, b21)))
   mut k2 = bigint_neg(bigint_add(bigint_mul(c1, b12), bigint_mul(c2, b22)))
   [k1, k2]
}
