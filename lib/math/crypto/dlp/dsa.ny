;; Keywords: dlp discrete-log group-theory dsa math crypto
;; Discrete-log routines for DSA nonce and signature analysis.
;; Reference:
;; - FIPS 186-4, Digital Signature Algorithm
;; References:
;; - std.math.crypto.dlp
;; - std.math.crypto
module std.math.crypto.dlp.dsa(dsa_sign_hash, dsa_verify_hash, dsa_recover_key_from_nonce, dsa_recover_nonce_reuse, dsa_recover_nonce_lcg_two_sigs, dsa_nonce_polynomial_value, dsa_verify_nonce_polynomial, dsa_recover_key_from_nonce_polynomial, dsa_recover_key_from_quadratic_nonces, dsa_verify_hash_or_bounds_zero_inverse_bug, dsa_zero_inverse_bypass_signature)
use std.math.nt
use std.math.matrix as matrix

fn dsa_sign_hash(any h, any x, any k, any p, any q, any g) any {
   "Sign integer hash h with DSA private key x and nonce k. Returns [r, s] or nil."
   def r = power_mod(g, k, p) % q
   if r == 0 { return nil }
   def kinv = inverse_mod(k, q)
   if kinv == nil || kinv == 0 { return nil }
   def s = mod(kinv * (Z(h) + Z(x) * r), q)
   if s == 0 { return nil }
   [r, s]
}

fn dsa_verify_hash(any h, list sig, any y, any p, any q, any g) bool {
   "Verify DSA signature [r, s] for integer hash h."
   def r, s = sig[0], sig[1]
   if r <= 0 || r >= q || s <= 0 || s >= q { return false }
   def w = inverse_mod(s, q)
   if w == nil || w == 0 { return false }
   def u1, u2 = mod(Z(h) * w, q), mod(r * w, q)
   def v = mod(mod(power_mod(g, u1, p) * power_mod(y, u2, p), p), q)
   v == r
}

fn dsa_verify_hash_or_bounds_zero_inverse_bug(any h, list sig, any y, any p, any q, any g) bool {
   "Model the common broken DSA verifier that checks `(r in range) or(s in range)`
   and treats a missing inverse as Python False/0. This accepts [1, 0]."
   def r, s = Z(sig[0]), Z(sig[1])
   if !((Z(0) < r && r < q) || (Z(0) < s && s < q)) { return false }
   def inv = inverse_mod(s, q)
   def w = (inv == nil || inv == Z(0)) ? Z(0) : inv
   def u1, u2 = mod(Z(h) * w, q), mod(r * w, q)
   def v = mod(mod(power_mod(g, u1, p) * power_mod(y, u2, p), p), q)
   v == r
}

fn dsa_zero_inverse_bypass_signature() list {
   "Return the malformed [r, s] signature accepted by the zero-inverse DSA verifier bug."
   [Z(1), Z(0)]
}

fn dsa_recover_key_from_nonce(any h, any r, any s, any k, any q) any {
   "Recover DSA private key x from one signature and known nonce k."
   def rinv = inverse_mod(r, q)
   if rinv == nil || rinv == 0 { return nil }
   mod((Z(s) * Z(k) - Z(h)) * rinv, q)
}

fn dsa_recover_nonce_reuse(any h1, any r1, any s1, any h2, any r2, any s2, any q) any {
   "Recover [k, x] from two DSA signatures that reused the same nonce."
   if r1 != r2 { return nil }
   def den = mod(Z(s1) - Z(s2), q)
   def inv = inverse_mod(den, q)
   if inv == nil || inv == 0 { return nil }
   def k = mod((Z(h1) - Z(h2)) * inv, q)
   def x = dsa_recover_key_from_nonce(h1, r1, s1, k, q)
   if x == nil { return nil }
   [k, x]
}

fn dsa_recover_nonce_lcg_two_sigs(any h1, any r1, any s1, any h2, any r2, any s2, any a, any c, any q) any {
   "Recover [k1, k2, x] when two DSA nonces are consecutive outputs of
   k2 = a*k1 + c(mod q)."
   def A, B = mod(Z(a) * Z(s2) * Z(r1) - Z(s1) * Z(r2), q), mod(Z(h2) * Z(r1) - Z(h1) * Z(r2) - Z(s2) * Z(c) * Z(r1), q)
   def inv = inverse_mod(A, q)
   if inv == nil || inv == 0 { return nil }
   def k1 = mod(B * inv, q)
   def k2 = mod(Z(a) * k1 + Z(c), q)
   def x = dsa_recover_key_from_nonce(h1, r1, s1, k1, q)
   if x == nil { return nil }
   [k1, k2, x]
}

fn dsa_nonce_polynomial_value(list coeffs, any i, any q) any {
   "Evaluate a nonce polynomial at index i modulo q.
   coeffs are low-to-high: [a0, a1, a2, ...]."
   mut acc = Z(0)
   mut pow_i = Z(1)
   def ii = Z(i)
   mut j = 0
   while j < coeffs.len {
      acc = mod(acc + Z(coeffs[j]) * pow_i, q)
      pow_i = mod(pow_i * ii, q)
      j += 1
   }
   acc
}

fn dsa_verify_nonce_polynomial(list records, any x, list coeffs, any q) bool {
   "Verify records [i, h, r, s] against DSA's s*k = h + x*r equation
   where k is a polynomial in i."
   mut n = 0
   while n < records.len {
      def rec = records[n]
      def i = rec[0]
      def h = Z(rec[1])
      def r = Z(rec[2])
      def s = Z(rec[3])
      def k = dsa_nonce_polynomial_value(coeffs, i, q)
      if mod(s * k - h - Z(x) * r, q) != Z(0) { return false }
      n += 1
   }
   true
}

fn dsa_recover_key_from_nonce_polynomial(list records, int degree, any q) any {
   "Recover [x, coeffs] when DSA nonces follow a degree-d polynomial in
   the record index. records are [i, h, r, s].
   From s_i*k(i) = h_i + x*r_i and k(i)=a0+a1*i+...+ad*i^d:
   -r_i*x + s_i*a0 + s_i*i*a1 + ... + s_i*i^d*ad = h_i mod q"
   if records.len < degree + 2 { return nil }
   mut A, b = [], []
   mut row_i = 0
   while row_i < records.len {
      def rec = records[row_i]
      def idx = Z(rec[0])
      def h = Z(rec[1])
      def r = Z(rec[2])
      def s = Z(rec[3])
      mut row = [mod(Z(0) - r, q)]
      mut pow_idx = Z(1)
      mut j = 0
      while j <= degree {
         row = row.append(mod(s * pow_idx, q))
         pow_idx = mod(pow_idx * idx, q)
         j += 1
      }
      A, b = A.append(row), b.append(mod(h, q))
      row_i += 1
   }
   def sol = matrix.matrix_solve_mod(matrix.Matrix(A), b, q)
   if sol == nil { return nil }
   mut coeffs = []
   mut j = 0
   while j <= degree {
      coeffs = coeffs.append(mod(sol[j + 1], q))
      j += 1
   }
   [mod(sol[0], q), coeffs]
}

fn dsa_recover_key_from_quadratic_nonces(list records, any q) any {
   "Recover [x, [a0, a1, a2]] when DSA nonces are quadratic in record index."
   dsa_recover_key_from_nonce_polynomial(records, 2, q)
}

#main {
   def p, q = Z(23), Z(11)
   def g, x = Z(4), Z(7)
   def y = power_mod(g, x, p)
   def h1 = Z(3)
   def h2 = Z(8)
   def k = Z(1)
   def sig1 = dsa_sign_hash(h1, x, k, p, q, g)
   def sig2 = dsa_sign_hash(h2, x, k, p, q, g)
   assert(sig1 != nil && sig2 != nil, "dsa sign")
   assert(dsa_verify_hash(h1, sig1, y, p, q, g), "dsa verify")
   def reuse = dsa_recover_nonce_reuse(h1, sig1[0], sig1[1], h2, sig2[0], sig2[1], q)
   assert(reuse != nil && reuse[0] == k && reuse[1] == x, "dsa nonce reuse")
   def a, c = Z(2), Z(1)
   def k1 = Z(1)
   def k2 = mod(a * k1 + c, q)
   def s1 = dsa_sign_hash(h1, x, k1, p, q, g)
   def s2 = dsa_sign_hash(h2, x, k2, p, q, g)
   def lcg = dsa_recover_nonce_lcg_two_sigs(h1, s1[0], s1[1], h2, s2[0], s2[1], a, c, q)
   assert(lcg != nil && lcg[0] == k1 && lcg[1] == k2 && lcg[2] == x, "dsa lcg nonce")
   def fq, fx = Z(101), Z(17)
   def coeffs = [Z(3), Z(5), Z(7)]
   def records = [
      [1, Z(8), Z(11), Z(13)],
      [2, Z(14), Z(19), Z(23)],
      [3, Z(99), Z(29), Z(31)],
      [4, Z(58), Z(37), Z(41)],
      [5, Z(23), Z(43), Z(47)]
   ]
   assert(dsa_verify_nonce_polynomial(records, fx, coeffs, fq), "dsa quadratic verify")
   def recovered = dsa_recover_key_from_quadratic_nonces(records, fq)
   assert(recovered != nil && recovered[0] == fx, "dsa quadratic key")
   assert(recovered[1] == coeffs, "dsa quadratic coeffs")
   print("✓ std.math.crypto.dlp.dsa self-test passed")
}
