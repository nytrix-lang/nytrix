;; Keywords: ecc elgamal math crypto public-key
;; EC ElGamal encryption, decryption, and recovery routines.
;; Decrypt, nonce reuse, key recovery
;; Reference:
;; - https://www.secg.org/sec1-v2.pdf
;; - https://www.rfc-editor.org/rfc/rfc8032
;; References:
;; - std.math.crypto.ecc
;; - std.math.crypto
module std.math.crypto.ecc.elgamal(elgamal_public_key, elgamal_private_key, elgamal_keygen, elgamal_encrypt, elgamal_decrypt, elgamal_derive_shared, elgamal_nonce_reuse, elgamal_recover_key_from_nonce_reuse, elgamal_key_nonce_reuse, elgamal_key_nonce_reuse_all, elgamal_recover_plaintext_from_nonce_reuse, elgamal_multiply_ciphertext, elgamal_unsafe_generator_leak, elgamal_sign, elgamal_verify, elgamal_signature_nonce_reuse)
use std.math.nt

fn elgamal_public_key(any h, any p, any g, any q=nil) list {
   "Create an ElGamal public key tuple [h, p, g, q]."
   [Z(h), Z(p), Z(g), q == nil ? Z(p) - Z(1) : Z(q)]
}

fn elgamal_private_key(any x, any p, any g, any q=nil) list {
   "Create an ElGamal private key tuple [x, p, g, q]."
   [Z(x), Z(p), Z(g), q == nil ? Z(p) - Z(1) : Z(q)]
}

fn elgamal_keygen(any p=nil, any g=nil, any q=nil, any x=nil) list {
   "Generate an ElGamal [public, private] key pair. x may be supplied for deterministic tests."
   if p == nil { p = (Z(1) << Z(1024)) - Z(1093337) }
   if g == nil { g = Z(7) }
   if q == nil { q = Z(p) - Z(1) }
   if x == nil { x = randint(Z(2), Z(p) - Z(2)) }
   def h = power_mod(g, x, p)
   [elgamal_public_key(h, p, g, q), elgamal_private_key(x, p, g, q)]
}

fn elgamal_derive_shared(list pubkey, any y) any {
   "Derive ElGamal shared secret h^y mod p from a public key."
   power_mod(pubkey[0], y, pubkey[1])
}

fn elgamal_encrypt(any m, list pubkey, any y=nil) list {
   "Encrypt integer message m with ElGamal public key [h, p, g, q]. Returns [c1, c2]."
   def h, p = pubkey[0], pubkey[1]
   def g = pubkey[2]
   if y == nil { y = randint(Z(2), Z(p) - Z(2)) }
   def c1 = power_mod(g, y, p)
   def s = power_mod(h, y, p)
   [c1, mod(Z(m) * s, p)]
}

fn elgamal_decrypt(any c1, any c2, any x=nil, any p=nil) any {
   "Decrypt an ElGamal ciphertext(c1, c2) using private key x modulo p. Returns the plaintext m."
   if is_list(c1) {
      def ct = c1
      def priv = c2
      return elgamal_decrypt(ct[0], ct[1], priv[0], priv[1])
   }
   def s = power_mod(c1, x, p)
   def s_inv = inverse_mod(s, p)
   (c2 * s_inv) % p
}

fn elgamal_nonce_reuse(any c1, any c2_1, any m1, any c2_2, any m2, any p) any {
   "Detect ElGamal nonce reuse by comparing shared secret s from two ciphertext/message pairs. " +
   "Returns nil when s values match (reused nonce), else [s1, s2]."
   def s1, s2 = (c2_1 * inverse_mod(m1, p)) % p, (c2_2 * inverse_mod(m2, p)) % p
   if s1 == s2 { nil } else { [s1, s2] }
}

fn elgamal_recover_plaintext_from_nonce_reuse(any p, any known_m, any known_c1, any known_c2, any target_c1, any target_c2) any {
   "Recover a target plaintext when ElGamal reused the same nonce/shared secret as a known plaintext.
   Returns nil if c1 differs or known_m is not invertible modulo p."
   if known_c1 != target_c1 { return nil }
   if gcd(known_m, p) != 1 { return nil }
   def s = mod(known_c2 * inverse_mod(known_m, p), p)
   if gcd(s, p) != 1 { return nil }
   mod(target_c2 * inverse_mod(s, p), p)
}

fn elgamal_multiply_ciphertext(list ct, any multiplier, any p) list {
   "Exploit ElGamal multiplicative malleability: ciphertext for m becomes ciphertext for m*multiplier mod p.
   Leaves c1 unchanged and multiplies c2 by multiplier."
   [ct[0], mod(ct[1] * multiplier, p)]
}

fn elgamal_recover_key_from_nonce_reuse(any c1, any c2_1, any m1, any c2_2, any m2, any p, any g) any {
   "Recover ElGamal private key x from two ciphertexts that reused nonce, given g and p. " +
   "Returns x or nil."
   def s1 = (c2_1 * inverse_mod(m1, p)) % p
   def k = dlog_brute(g, s1, p, p)
   if k == nil { return nil }
   def x = dlog_brute(g, power_mod(c1, inverse_mod(k % (p - 1), p - 1), p), p, p)
   x
}

fn elgamal_key_nonce_reuse(any p, any m1, any r1, any s1, any m2, any r2, any s2) any {
   "Recover [k, x] from two ElGamal signatures that reused the same nonce."
   if r1 != r2 { return nil }
   def pm1 = p - 1
   def sdiff = mod(s1 - s2, pm1)
   if gcd(sdiff, pm1) != 1 { return nil }
   def k = mod((m1 - m2) * inverse_mod(sdiff, pm1), pm1)
   if gcd(r1, pm1) != 1 { return nil }
   def x = mod((m1 - k * s1) * inverse_mod(r1, pm1), pm1)
   [k, x]
}

fn elgamal_key_nonce_reuse_all(any p, any m1, any r1, any s1, any m2, any r2, any s2) list {
   "Recover all [k, x] candidates from two ElGamal signatures that reused a nonce.
   Handles non-coprime linear congruence cases by returning every valid candidate."
   if r1 != r2 { return [] }
   def pm1 = p - 1
   def ks = solve_linear_congruence(s1 - s2, m1 - m2, pm1)
   mut out = []
   mut i = 0
   while i < ks.len {
      def k = ks[i]
      def xs = solve_linear_congruence(r1, m1 - k * s1, pm1)
      mut j = 0
      while j < xs.len {
         out = out.append([mod(k, pm1), mod(xs[j], pm1)])
         j += 1
      }
      i += 1
   }
   out
}

fn elgamal_unsafe_generator_leak(any p, any h, any c1, any c2) any {
   "Returns the Legendre-symbol leakage bit for unsafe-generator ElGamal."
   def lh = legendre(Z(h), Z(p))
   def lc1 = legendre(Z(c1), Z(p))
   if lh == 0 || lc1 == 0 { return nil }
   def lc2 = legendre(Z(c2), Z(p))
   lc2 / max(lh, lc1)
}

fn elgamal_sign(any m, any x, any p, any g, any k) any {
   "Create ElGamal signature [r, s] for message representative m.
   Requires gcd(k, p-1) = 1. Returns nil if k is invalid."
   def pm1 = p - 1
   if gcd(k, pm1) != 1 { return nil }
   def r = power_mod(g, k, p)
   if gcd(r, pm1) != 1 { return nil }
   def kinv = inverse_mod(k, pm1)
   def s = ((m - x * r) * kinv) % pm1
   [r, mod(s, pm1)]
}

fn elgamal_verify(any m, list sig, any y, any p, any g) bool {
   "Verify ElGamal signature [r, s] on message representative m."
   def r, s = sig[0], sig[1]
   if r <= 0 || r >= p { return false }
   if s <= 0 || s >= p - 1 { return false }
   def lhs, rhs = power_mod(g, m, p), (power_mod(y, r, p) * power_mod(r, s, p)) % p
   lhs == rhs
}

fn elgamal_signature_nonce_reuse(list sig1, any m1, list sig2, any m2, any p) any {
   "Recover [k, x] from two ElGamal signatures that reused the same nonce.
   This implementation handles the common invertible case where
   gcd(s1-s2, p-1) = gcd(r, p-1) = 1."
   def r1, s1 = sig1[0], sig1[1]
   def r2, s2 = sig2[0], sig2[1]
   if r1 != r2 { return nil }
   def pm1 = p - 1
   def sdiff = mod(s1 - s2, pm1)
   if gcd(sdiff, pm1) != 1 { return nil }
   def k = mod((m1 - m2) * inverse_mod(sdiff, pm1), pm1)
   if gcd(r1, pm1) != 1 { return nil }
   def x = mod((m1 - s1 * k) * inverse_mod(r1, pm1), pm1)
   [k, x]
}

fn dlog_brute(any g, any h, any p, any max_iter) any {
   "Internal brute-force DLP solver for g^x = h(mod p), up to max_iter iterations. " +
   "Returns x if found, else nil."
   mut val = 1
   mut x = 0
   while x < max_iter {
      if val == h { return x }
      val = (val * g) % p
      x += 1
   }
   nil
}

#main {
   def p, g = 23, 5
   def x, h = 6, power_mod(g, x, p)
   def m, k = 7, 3
   def c1 = power_mod(g, k, p)
   def c2 = (m * power_mod(h, k, p)) % p
   assert(elgamal_decrypt(c1, c2, x, p) == m, "elgamal_decrypt")
   def pub = elgamal_public_key(h, p, g)
   def ct = elgamal_encrypt(m, pub, k)
   assert(elgamal_decrypt(ct[0], ct[1], x, p) == m, "elgamal_encrypt/decrypt")
   def m2 = 11
   def c2b = (m2 * power_mod(h, k, p)) % p
   def reuse = elgamal_nonce_reuse(c1, c2, m, c2b, m2, p)
   assert(reuse == nil, "nonce reuse detected")
   def k2 = 4
   def c2c = (m2 * power_mod(h, k2, p)) % p
   def no_reuse = elgamal_nonce_reuse(c1, c2, m, c2c, m2, p)
   assert(no_reuse == [6, 2], "nonce reuse exact")
   def ksig = 7
   def sig = elgamal_sign(m, x, p, g, ksig)
   assert(sig != nil, "elgamal_sign")
   assert(elgamal_verify(m, sig, h, p, g), "elgamal_verify")
   def mal = elgamal_multiply_ciphertext(ct, 3, p)
   assert(elgamal_decrypt(mal[0], mal[1], x, p) == (m * 3) % p, "malleability")
   print("✓ std.math.crypto.ecc.elgamal self-test passed")
}
