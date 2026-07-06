;; Keywords: public-key damgard-jurik paillier generalization homomorphic encryption math crypto
;; Damgard-Jurik encryption, decryption — a generalization of Paillier to modulus n^{s+1}.
;; When s = 1 the scheme reduces to standard Paillier.
;; Reference:
;; - I. Damgard, M. Jurik, A Generalisation, a Simplification and Some Applications of
;;   Paillier's Probabilistic Public-Key System.
;; References:
;; - std.math.crypto.public_key
;; - std.math.crypto
;; - inspiration from crypto-commons (asymmetric)
module std.math.crypto.public_key.damgard_jurik(damgard_jurik_keygen, damgard_jurik_encrypt, damgard_jurik_decrypt)
use std.core
use std.math.nt
use std.math.crypto.public_key.paillier (paillier_encrypt)

fn _dj_l(any u, any n) bigint {
   (Z(u) - Z(1)) / Z(n)
}

fn _dj_require_coprime(any r, any n, str name) int {
   if gcd(Z(r), Z(n)) != Z(1) { panic("damgard_jurik: " + name + " must be coprime to n") }
   0
}

fn _dj_fact(int k) bigint {
   mut f, i = Z(1), 2
   while i <= k {
      f = f * Z(i)
      i += 1
   }
   f
}

fn _dj_decrypt(any ct, any d, any n, int s) bigint {
   def nn = Z(n)
   def ns1 = pow(nn, Z(s + 1))
   def a = power_mod(Z(ct), Z(d), ns1)
   mut i, j = Z(0), 1
   while j <= s {
      def nj1 = pow(nn, Z(j + 1))
      mut t1, t2 = _dj_l(mod(a, nj1), nn), i
      mut k = 2
      while k <= j {
         i = i - Z(1)
         t2 = mod(t2 * i, pow(nn, Z(j)))
         def fac = _dj_fact(k)
         def up = mod(t2 * pow(nn, Z(k - 1)), pow(nn, Z(j)))
         def down = inverse_mod(fac, pow(nn, Z(j)))
         t1 = mod(t1 - mod(up * down, pow(nn, Z(j))), pow(nn, Z(j)))
         k += 1
      }
      i = t1
      j += 1
   }
   i
}

fn damgard_jurik_keygen(any p, any q, int s=1, any g=nil) dict {
   "Build a Damgard-Jurik key dictionary from primes p, q and plaintext-block parameter s.
   The default generator is n + 1. The key encodes n^s and n^{s+1} for efficiency."
   def pp = Z(p)
   def qq = Z(q)
   if pp <= Z(2) || qq <= Z(2) { panic("damgard_jurik: p and q must be odd primes") }
   if pp == qq { panic("damgard_jurik: p and q must be distinct") }
   def n = pp * qq
   def ns = pow(n, Z(s))
   def ns1 = pow(n, Z(s + 1))
   def gg = g == nil ? n + Z(1) : Z(g)
   def lam = lcm(pp - Z(1), qq - Z(1))
   def jd = _dj_decrypt(gg, lam, n, s)
   def jd_inv = inverse_mod(jd, ns)
   if jd_inv == Z(0) { panic("damgard_jurik: invalid generator") }
   mut key = dict(8)
   key["scheme"] = "Damgard-Jurik"
   key["p"] = pp
   key["q"] = qq
   key["n"] = n
   key["s"] = s
   key["ns"] = ns
   key["ns1"] = ns1
   key["g"] = gg
   key["lambda"] = lam
   key["jd_inv"] = jd_inv
   key
}

fn damgard_jurik_encrypt(any m, dict key, any r=nil) bigint {
   "Encrypt plaintext m with a Damgard-Jurik key. The nonce r is generated randomly
   when not provided (deterministic nonces are useful for tests)."
   def n, s = Z(key.get("n")), int(key.get("s"))
   def ns, ns1 = Z(key.get("ns")), Z(key.get("ns1"))
   def g = Z(key.get("g"))
   def rr = r == nil ? randint(Z(2), ns1) : Z(r)
   _dj_require_coprime(rr, n, "r")
   mod(power_mod(g, Z(m), ns1) * power_mod(rr, ns, ns1), ns1)
}

fn damgard_jurik_decrypt(any c, dict key) bigint {
   "Decrypt a Damgard-Jurik ciphertext with the key dictionary."
   def n, s = Z(key.get("n")), int(key.get("s"))
   def lam, jd_inv = Z(key.get("lambda")), Z(key.get("jd_inv"))
   def ns = Z(key.get("ns"))
   def jc = _dj_decrypt(Z(c), lam, n, s)
   mod(jc * jd_inv, ns)
}

#main {
   def key1 = damgard_jurik_keygen(7, 11, 1)
   assert(key1.get("scheme") == "Damgard-Jurik", "dj scheme name")
   assert(key1.get("n") == Z(77), "dj s=1 modulus")
   def c12 = damgard_jurik_encrypt(12, key1, 5)
   assert(paillier_encrypt(12, key1.get("n"), key1.get("g"), 5) == c12, "dj s=1 matches paillier encrypt")
   def m12 = damgard_jurik_decrypt(c12, key1)
   assert(m12 == Z(12), "dj s=1 round-trip")

   def c20, m20 = damgard_jurik_encrypt(20, key1, 13), damgard_jurik_decrypt(c20, key1)
   assert(m20 == Z(20), "dj s=1 second round-trip")

   def key2 = damgard_jurik_keygen(7, 11, 2)
   assert(key2.get("ns") == Z(77 * 77), "dj s=2 n^2")
   assert(key2.get("ns1") == Z(77 * 77 * 77), "dj s=2 n^3")
   def c2_12, m2_12 = damgard_jurik_encrypt(12, key2, 5), damgard_jurik_decrypt(c2_12, key2)
   assert(m2_12 == Z(12), "dj s=2 round-trip")

   def key3 = damgard_jurik_keygen(11, 13, 3)
   def c3 = damgard_jurik_encrypt(42, key3, 7)
   def m3 = damgard_jurik_decrypt(c3, key3)
   assert(m3 == Z(42), "dj s=3 round-trip")

   print("✓ std.math.crypto.public_key.damgard_jurik self-test passed")
}
