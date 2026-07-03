;; Keywords: public-key rabin rabin-williams square-root encryption signature math crypto
;; Rabin encryption, square-root decryption, Rabin-Williams signatures, and helpers.
;; Reference:
;; - M. O. Rabin, Digitalized Signatures and Public-Key Functions as Intractable as Factorization.
;; - H. C. Williams, A Modification of the RSA Public-Key Encryption Procedure.
;; References:
;; - std.math.crypto.public_key
;; - std.math.crypto
module std.math.crypto.public_key.rabin(
   rabin_keygen, rabin_encrypt, rabin_valid_blum_ciphertext,
   rabin_principal_decrypt_blum, rabin_decrypt_candidates,
   rabin_select_candidate, rabin_decrypt_with_padding,
   rabin_sign_int, rabin_verify_int, rabin_sign, rabin_verify
)
use std.core
use std.math.nt
use std.math.crypto.hash (sha256_bytes)

fn _rabin_append_unique(list xs, any x) list {
   xs.contains(x) ? xs : xs.append(x)
}

fn rabin_keygen(any p, any q) dict {
   "Build a Rabin key dictionary from distinct odd primes p and q."
   def pp = Z(p)
   def qq = Z(q)
   if pp == qq { panic("rabin: p and q must be distinct") }
   if pp <= Z(2) || qq <= Z(2) || !is_prime(pp) || !is_prime(qq) {
      panic("rabin: p and q must be odd primes")
   }
   mut key = dict(5)
   key["scheme"] = "Rabin"
   key["p"] = pp
   key["q"] = qq
   key["n"] = pp * qq
   key["blum"] = mod(pp, Z(4)) == Z(3) && mod(qq, Z(4)) == Z(3)
   key
}

fn rabin_encrypt(any m, any n) bigint {
   "Encrypt m as m^2 mod n."
   def nn = Z(n)
   mod(Z(m) * Z(m), nn)
}

fn rabin_valid_blum_ciphertext(any c, any p, any q) bool {
   "Return true when c is invertible and a quadratic residue modulo both Blum primes."
   def pp = Z(p)
   def qq = Z(q)
   def n = pp * qq
   def cc = mod(Z(c), n)
   gcd(cc, n) == Z(1) && legendre(cc, pp) == 1 && legendre(cc, qq) == 1
}

fn rabin_principal_decrypt_blum(any c, any p, any q) any {
   "Return the principal Rabin square root used by many Blum-integer CTF services.
   Returns nil when c is not an invertible quadratic residue modulo p*q."
   if !rabin_valid_blum_ciphertext(c, p, q) { return nil }
   def pp = Z(p)
   def qq = Z(q)
   def exp = ((pp - Z(1)) * (qq - Z(1)) + Z(4)) / Z(8)
   power_mod(Z(c), exp, pp * qq)
}

fn rabin_decrypt_candidates(any c, any p, any q) list {
   "Return all square roots of c modulo n = p*q.
   Rabin decryption is ambiguous; callers normally select a candidate with
   padding or a known prefix."
   def pp = Z(p)
   def qq = Z(q)
   def cp = mod(Z(c), pp)
   def cq = mod(Z(c), qq)
   def rp = tonelli_shanks(cp, pp)
   def rq = tonelli_shanks(cq, qq)
   if rp == Z(-1) || rq == Z(-1) { return [] }
   def roots_p, roots_q = _rabin_append_unique([rp], mod(-rp, pp)), _rabin_append_unique([rq], mod(-rq, qq))
   mut out = []
   mut i = 0
   while i < roots_p.len {
      mut j = 0
      while j < roots_q.len {
         def x = crt([roots_p.get(i), roots_q.get(j)], [pp, qq])
         if x != nil { out = _rabin_append_unique(out, x) }
         j += 1
      }
      i += 1
   }
   out
}

fn rabin_select_candidate(list candidates, any low=nil, any high=nil, any residue=nil, any modulus=nil) any {
   "Select one candidate by optional range and residue filters.
   Returns nil if no candidate matches and panics if more than one remains."
   mut matches = []
   mut i = 0
   while i < candidates.len {
      def x = Z(candidates.get(i))
      def ok_low = low == nil || x >= Z(low)
      def ok_high = high == nil || x <= Z(high)
      def ok_residue = residue == nil || modulus == nil || mod(x, Z(modulus)) == mod(Z(residue), Z(modulus))
      if ok_low && ok_high && ok_residue { matches = matches.append(x) }
      i += 1
   }
   if matches.len == 0 { return nil }
   if matches.len > 1 { panic("rabin: padding filter is ambiguous") }
   matches.get(0)
}

fn rabin_decrypt_with_padding(any c, dict key, any low=nil, any high=nil, any residue=nil, any modulus=nil) any {
   "Decrypt c and select the root that matches a simple padding predicate."
   rabin_select_candidate(rabin_decrypt_candidates(c, key.get("p"), key.get("q")), low, high, residue, modulus)
}

fn rabin_sign_int(any m, dict key) list {
   "Sign integer representative m with Rabin-Williams.
   Returns [s, tweak] where s is a principal square root of m + tweak mod n.
   At most 256 tweaks are tried to make m + tweak a quadratic residue."
   def pp = Z(key.get("p"))
   def qq = Z(key.get("q"))
   def n = Z(key.get("n"))
   mut tweak = 0
   while tweak < 256 {
      def c, s = mod(Z(m) + tweak, n), rabin_principal_decrypt_blum(c, pp, qq)
      if s != nil { return [s, tweak] }
      tweak += 1
   }
   panic("rabin_sign_int: message is not QR-representable after 256 tweaks")
}

fn rabin_verify_int(any m, list sig, any n) bool {
   "Verify a Rabin-Williams signature [s, tweak] on integer representative m.
   Returns true iff s^2 ≡ m + tweak (mod n)."
   def s, tweak = Z(sig.get(0)), Z(sig.get(1))
   def nn = Z(n)
   mod(s * s, nn) == mod(Z(m) + tweak, nn)
}

fn _rabin_hash_int(str msg) bigint {
   def bs = sha256_bytes(msg)
   mut m, i = Z(0), 0
   while i < bs.len {
      m = m * Z(256) + Z(bs.get(i))
      i += 1
   }
   m
}

fn rabin_sign(str msg, dict key) list {
   "Hash msg with SHA-256, then sign the integer hash with Rabin-Williams."
   rabin_sign_int(_rabin_hash_int(msg), key)
}

fn rabin_verify(str msg, list sig, any n) bool {
   "Verify a Rabin-Williams signature [s, tweak] on msg using public modulus n."
   rabin_verify_int(_rabin_hash_int(msg), sig, n)
}

#main {
   def key = rabin_keygen(7, 11)
   assert(key.get("n") == Z(77), "rabin modulus")
   assert(key.get("blum"), "rabin blum key")
   def c = rabin_encrypt(20, key.get("n"))
   assert(c == Z(15), "rabin ciphertext")
   def roots = rabin_decrypt_candidates(c, 7, 11)
   assert(roots.len == 4, "rabin four roots")
   assert(roots.contains(Z(20)), "rabin roots include message")
   assert(roots.contains(Z(57)), "rabin roots include negated message")
   assert(rabin_valid_blum_ciphertext(c, 7, 11), "rabin valid ciphertext")
   assert(roots.contains(rabin_principal_decrypt_blum(c, 7, 11)), "rabin principal root is a candidate")
   assert(rabin_principal_decrypt_blum(2, 7, 11) == nil, "rabin invalid ciphertext")
   assert(rabin_decrypt_with_padding(c, key, 18, 22) == Z(20), "rabin range padding")
   assert(rabin_decrypt_with_padding(c, key, nil, nil, 2, 9) == Z(20), "rabin residue padding")
   assert(rabin_decrypt_with_padding(c, key, 1, 3) == nil, "rabin no padding match")

   def sig = rabin_sign_int(5, key)
   def s, t = Z(sig.get(0)), Z(sig.get(1))
   assert(rabin_verify_int(5, sig, key.get("n")), "rabin-williams verify int")
   assert(mod(s * s, key.get("n")) == mod(Z(5) + t, key.get("n")), "rabin-williams signature property")
   assert(!rabin_verify_int(6, sig, key.get("n")), "rabin-williams wrong message")

   def msg_sig = rabin_sign("hello", key)
   assert(rabin_verify("hello", msg_sig, key.get("n")), "rabin-williams verify string")
   assert(!rabin_verify("world", msg_sig, key.get("n")), "rabin-williams wrong string")

   print("✓ std.math.crypto.public_key.rabin self-test passed")
}
