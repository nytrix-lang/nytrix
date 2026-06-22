;; Keywords: public-key blum-goldwasser math crypto
;; Blum-Goldwasser operations.
;; Reference:
;; - https://doi.org/10.1007/3-540-39568-7_9
;; References:
;; - std.math.crypto.public_key
;; - std.math.crypto
module std.math.crypto.public_key.blum_goldwasser(bg_keygen, bg_random_keygen, bg_seed_state, bg_encrypt_bits, bg_decrypt_bits, bg_encrypt_int, bg_decrypt_int)
use std.core
use std.math.nt
use std.math.crypto.number.arith
use std.math.crypto.number.pseudoprimes

fn _bg_require_blum_prime(any p, str name) int {
   if !is_blum_prime(p) { panic("blum_goldwasser: " + name + " must be a Blum prime") }
   0
}

fn _bg_bits_checked(list bits) list {
   mut out = []
   mut i = 0
   while i < bits.len {
      def b = bits.get(i)
      if b != 0 && b != 1 { panic("blum_goldwasser: plaintext bits must be 0 or 1") }
      out = out.append(b)
      i += 1
   }
   out
}

fn _bg_seed_default(any n) bigint {
   mut s = Z(3)
   while gcd(s, n) != Z(1) { s += Z(2) }
   s
}

fn bg_seed_state(any seed, any n) bigint {
   "Returns the quadratic-residue start state derived from `seed` modulo `n`."
   def nn = Z(n)
   def s = seed == nil ? _bg_seed_default(nn) : Z(seed)
   if gcd(s, nn) != Z(1) { panic("blum_goldwasser: seed must be coprime to n") }
   mod(s * s, nn)
}

fn bg_keygen(any p, any q) dict {
   "Builds a Blum-Goldwasser key dictionary from Blum primes `p` and `q`."
   _bg_require_blum_prime(p, "p")
   _bg_require_blum_prime(q, "q")
   if Z(p) == Z(q) { panic("blum_goldwasser: p and q must be distinct") }
   mut key = dict(4)
   key["p"] = Z(p)
   key["q"] = Z(q)
   key["n"] = Z(p) * Z(q)
   key["scheme"] = "Blum-Goldwasser"
   key
}

fn bg_random_keygen(int bits=32) dict {
   "Generate a small Blum-Goldwasser key for deterministic validation."
   def half = max(4, bits / 2)
   def lo = Z(1) << Z(half - 1)
   def hi = (Z(1) << Z(half)) - Z(1)
   mut p, q = random_blum_prime(lo, hi), random_blum_prime(lo, hi)
   while p == q { q = random_blum_prime(lo, hi) }
   bg_keygen(p, q)
}

fn _bg_step(any x, any n) bigint { mod(Z(x) * Z(x), Z(n)) }

fn bg_encrypt_bits(list bits, any n, any seed=nil) dict {
   "Encrypts a bit list with public modulus `n`; returns ciphertext bits and final state."
   def plain = _bg_bits_checked(bits)
   def nn = Z(n)
   mut x = bg_seed_state(seed, nn)
   mut ct = []
   mut i = 0
   while i < plain.len {
      x = _bg_step(x, nn)
      ct = ct.append((plain.get(i) & 1) ^^ bigint_to_int(x % Z(2)))
      i += 1
   }
   mut out = dict(4)
   out["bits"] = ct
   out["final"] = x
   out["n"] = nn
   out["len"] = plain.len
   out
}

fn _bg_rewind_exp(any p, int steps) bigint { power_mod((Z(p) + Z(1)) / Z(4), Z(steps), Z(p) - Z(1)) }

fn _bg_recover_start(any final_state, any p, any q, int steps) bigint {
   def pp = Z(p)
   def qq = Z(q)
   def ep = _bg_rewind_exp(pp, steps)
   def eq = _bg_rewind_exp(qq, steps)
   def xp = power_mod(final_state, ep, pp)
   def xq = power_mod(final_state, eq, qq)
   crt([xp, xq], [pp, qq])
}

fn bg_decrypt_bits(dict cipher, any p, any q) list {
   "Decrypts a Blum-Goldwasser ciphertext dictionary using secret primes `p` and `q`."
   _bg_require_blum_prime(p, "p")
   _bg_require_blum_prime(q, "q")
   def bits = cipher.get("bits", [])
   def steps = cipher.get("len", bits.len)
   def n = Z(p) * Z(q)
   mut x = _bg_recover_start(cipher.get("final"), p, q, steps)
   mut out = []
   mut i = 0
   while i < bits.len {
      x = _bg_step(x, n)
      out = out.append((bits.get(i) & 1) ^^ bigint_to_int(x % Z(2)))
      i += 1
   }
   out
}

fn bg_encrypt_int(any m, int bit_count, any n, any seed=nil) dict {
   "Encrypts the `bit_count` least-significant bits of integer `m`."
   bg_encrypt_bits(int_to_bits_le(m, bit_count), n, seed)
}

fn bg_decrypt_int(dict cipher, any p, any q) any {
   "Decrypts a Blum-Goldwasser integer ciphertext."
   bits_to_int_le(bg_decrypt_bits(cipher, p, q))
}

#main {
   def key = bg_keygen(7, 11)
   def n = key.get("n")
   def msg_bits = [1, 0, 1, 1, 0, 0, 1]
   def cipher = bg_encrypt_bits(msg_bits, n, 5)
   assert(cipher.get("bits") == [0, 0, 1, 0, 1, 0, 1], "bg ciphertext bits")
   assert(cipher.get("final") == 16, "bg final state")
   assert(bg_decrypt_bits(cipher, 7, 11) == msg_bits, "bg bit decrypt")
   def int_cipher = bg_encrypt_int(45, 6, n, 5)
   assert(bg_decrypt_int(int_cipher, 7, 11) == 45, "bg int decrypt")
   def generated = bg_keygen(11, 19)
   assert(is_blum_prime(generated.get("p")), "generated p")
   assert(is_blum_prime(generated.get("q")), "generated q")
   assert(generated.get("p") != generated.get("q"), "generated distinct primes")
   def pp = generate_pseudoprime([Z(2)], Z(3), Z(5), 0)
   assert(pp == [Z(29341), Z(13), Z(37), Z(61)], "deterministic pseudoprime")
   assert(pp[0] == pp[1] * pp[2] * pp[3], "pseudoprime factors multiply")
   assert(!is_prime(pp[0]), "pseudoprime is composite")
   assert(power_mod(Z(2), pp[0] - Z(1), pp[0]) == Z(1), "base-2 Fermat pass")
   def n2 = bg_keygen(7, 11).get("n")
   assert(n2 == Z(77), "bg modulus")
   assert(bg_seed_state(5, n2) == Z(25), "bg seed state")
   def bits = [1, 0, 1, 1, 0, 0, 1]
   def cipher2 = bg_encrypt_bits(bits, n2, 5)
   assert(cipher2.get("bits") == [0, 0, 1, 0, 1, 0, 1], "bg deterministic bits")
   assert(cipher2.get("final") == Z(16), "bg final state")
   assert(bg_decrypt_bits(cipher2, 7, 11) == bits, "bg bit decrypt")
   def cipher_int = bg_encrypt_int(45, 6, n2, 5)
   assert(bg_decrypt_int(cipher_int, 7, 11) == 45, "bg integer decrypt")
   print("✓ std.math.crypto.public_key.blum_goldwasser self-test passed")
}
