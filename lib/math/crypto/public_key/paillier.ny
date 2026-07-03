;; Keywords: public-key paillier homomorphic encryption math crypto
;; Paillier encryption, decryption, and additive-homomorphic helpers.
;; Reference:
;; - P. Paillier, Public-Key Cryptosystems Based on Composite Degree Residuosity Classes.
;; References:
;; - std.math.crypto.public_key
;; - std.math.crypto
;; - tmp/crypto/inspiration/crypto-commons/crypto_commons/asymmetric/asymmetric.py
module std.math.crypto.public_key.paillier(paillier_keygen, paillier_encrypt, paillier_decrypt, paillier_encrypt_default, paillier_add, paillier_mul_plain, paillier_rerandomize)
use std.core
use std.math.nt

fn _paillier_l(any u, any n) bigint {
   (Z(u) - Z(1)) / Z(n)
}

fn _paillier_require_unit(any r, any n, str name) int {
   if gcd(Z(r), Z(n)) != Z(1) { panic("paillier: " + name + " must be coprime to n") }
   0
}

fn paillier_keygen(any p, any q, any g=nil) dict {
   "Build a Paillier key dictionary from prime factors p and q.
   The default generator is n + 1, which keeps encryption simple and reliable."
   def pp = Z(p)
   def qq = Z(q)
   if pp <= Z(2) || qq <= Z(2) { panic("paillier: p and q must be odd primes") }
   if pp == qq { panic("paillier: p and q must be distinct") }
   def n = pp * qq
   def n2 = n * n
   def gg = g == nil ? n + Z(1) : Z(g)
   def lam = lcm(pp - Z(1), qq - Z(1))
   def x = _paillier_l(power_mod(gg, lam, n2), n)
   def mu = inverse_mod(x, n)
   if mu == Z(0) { panic("paillier: invalid generator") }
   mut key = dict(8)
   key["scheme"] = "Paillier"
   key["p"] = pp
   key["q"] = qq
   key["n"] = n
   key["n2"] = n2
   key["g"] = gg
   key["lambda"] = lam
   key["mu"] = mu
   key
}

fn paillier_encrypt(any m, any n, any g, any r) bigint {
   "Encrypt plaintext m with public parameters n, g, and nonce r."
   def nn = Z(n)
   _paillier_require_unit(r, nn, "r")
   def n2 = nn * nn
   mod(power_mod(Z(g), mod(Z(m), nn), n2) * power_mod(Z(r), nn, n2), n2)
}

fn paillier_encrypt_default(any m, dict key, any r=Z(2)) bigint {
   "Encrypt m with a Paillier key dictionary. The caller may pass r for reproducible tests."
   paillier_encrypt(m, key.get("n"), key.get("g"), r)
}

fn paillier_decrypt(any c, dict key) bigint {
   "Decrypt a Paillier ciphertext with a key dictionary that contains lambda and mu."
   def n = key.get("n")
   def n2 = key.get("n2", n * n)
   def x = _paillier_l(power_mod(Z(c), key.get("lambda"), n2), n)
   mod(x * key.get("mu"), n)
}

fn paillier_add(any c1, any c2, any n) bigint {
   "Return an encryption of m1 + m2 from encryptions of m1 and m2."
   def n2 = Z(n) * Z(n)
   mod(Z(c1) * Z(c2), n2)
}

fn paillier_mul_plain(any c, any k, any n) bigint {
   "Return an encryption of k*m from an encryption of m."
   power_mod(Z(c), Z(k), Z(n) * Z(n))
}

fn paillier_rerandomize(any c, any n, any r) bigint {
   "Refresh a ciphertext without changing its plaintext."
   def nn = Z(n)
   _paillier_require_unit(r, nn, "r")
   def n2 = nn * nn
   mod(Z(c) * power_mod(Z(r), nn, n2), n2)
}

#main {
   def key = paillier_keygen(7, 11)
   assert(key.get("n") == Z(77), "paillier modulus")
   assert(key.get("g") == Z(78), "paillier default generator")
   assert(key.get("lambda") == Z(30), "paillier lambda")

   def c12 = paillier_encrypt_default(12, key, 5)
   assert(c12 == Z(4469), "paillier deterministic ciphertext")
   assert(paillier_decrypt(c12, key) == Z(12), "paillier decrypt")

   def c20 = paillier_encrypt_default(20, key, 13)
   def sum = paillier_add(c12, c20, key.get("n"))
   assert(paillier_decrypt(sum, key) == Z(32), "paillier homomorphic add")

   def scaled = paillier_mul_plain(c12, 3, key.get("n"))
   assert(paillier_decrypt(scaled, key) == Z(36), "paillier plaintext multiply")

   def fresh = paillier_rerandomize(c12, key.get("n"), 17)
   assert(fresh != c12, "paillier rerandomizes")
   assert(paillier_decrypt(fresh, key) == Z(12), "paillier rerandomized decrypt")

   def wrapped = paillier_encrypt_default(80, key, 5)
   assert(paillier_decrypt(wrapped, key) == Z(3), "paillier message reduced modulo n")
   print("✓ std.math.crypto.public_key.paillier self-test passed")
}
