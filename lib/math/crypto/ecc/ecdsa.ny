;; Keywords: ecc ecdsa math crypto public-key
;; Elliptic-curve routines for ECDSA signing and nonce-reuse analysis.
;; Reference:
;; - https://www.secg.org/sec1-v2.pdf
;; - https://eprint.iacr.org/2019/023.pdf (HNP via lattice)
;; References:
;; - std.math.crypto.ecc
;; - std.math.crypto
module std.math.crypto.ecc.ecdsa(ecdsa_nonce_reuse, ecdsa_recover_key_from_two_sigs, ecdsa_forge_with_known_k, ecdsa_partial_nonce_leak, ecdsa_sign, ecdsa_verify, ecdsa_private_key_from_nonce, ecdsa_nonce_from_private_key, ecdsa_hnp_parse_signature_hex, ecdsa_hnp_parse_line, ecdsa_hnp_sample_residual, ecdsa_hnp_lsb_sample, ecdsa_hnp_msb_sample, ecdsa_hnp_lsb_samples, ecdsa_hnp_msb_samples, ecdsa_hnp_lsb_line, ecdsa_hnp_msb_line, ecdsa_hnp_lsb_lines, ecdsa_hnp_msb_lines, ecdsa_hnp_residuals, ecdsa_hnp_residual_width, ecdsa_hnp_count_valid_samples, ecdsa_hnp_check_samples, ecdsa_hnp_check_lsb_lines, ecdsa_hnp_check_msb_lines, ecdsa_hnp_recover, ecdsa_recover_key_from_lsb_lines, ecdsa_recover_key_from_msb_lines, ecdsa_recover_key_from_nonce_lsb, ecdsa_recover_key_from_nonce_msb)
use std.math.nt
use std.math.crypto.ecc.ecc
use std.math.crypto.hnp.hnp as hnp
use std.core.str as str
use std.math.matrix as matrix

fn ecdsa_nonce_reuse(list sig1, list sig2, any msg1, any msg2, any curve_n) any {
   "Recover nonce k and private key d when two ECDSA signatures share the same k.
   sig1, sig2: [r, s] pairs. msg1, msg2: message hashes(integers).
   curve_n: curve order.
   Returns [k, d] or nil if r values differ or s-difference is zero."
   def r1, s1 = sig1[0], sig1[1]
   def r2, s2 = sig2[0], sig2[1]
   if r1 != r2 { return nil }
   def s_diff = ((s1 - s2) % curve_n + curve_n) % curve_n
   if s_diff == 0 { return nil }
   def k = ((((msg1 - msg2) % curve_n + curve_n) % curve_n) * inverse_mod(s_diff, curve_n)) % curve_n
   def r_inv = inverse_mod(r1, curve_n)
   def d = ((((s1 * k - msg1) % curve_n + curve_n) % curve_n) * r_inv) % curve_n
   [k, d]
}

fn ecdsa_recover_key_from_two_sigs(any r1, any s1, any m1, any r2, any s2, any m2, any curve_n) any {
   "Recover ECDSA private key d from two signatures with the same nonce k.
   r1,s1,m1: first(r,s,hash). r2,s2,m2: second(r,s,hash). curve_n: order.
   Returns [k, d] or nil."
   ecdsa_nonce_reuse([r1, s1], [r2, s2], m1, m2, curve_n)
}

fn ecdsa_forge_with_known_k(any m, any k, list G, any d, any a, any p, any curve_n) any {
   "Forge an ECDSA signature given the private key d and nonce k.
   m: message hash. k: nonce. G: base point [x,y]. d: private key.
   a, p: curve parameters. curve_n: curve order.
   Returns [r, s] or nil if r or s is zero."
   def R = ecc_scalar_mult(k, G, a, p)
   if R == nil { return nil }
   def r = R[0] % curve_n
   if r == 0 { return nil }
   def k_inv = inverse_mod(k, curve_n)
   def s = k_inv * ((m + r * d) % curve_n) % curve_n
   if s == 0 { return nil }
   [r, s]
}

fn ecdsa_sign(any m, any k, list G, any d, any a, any p, any curve_n) any {
   "Compute ECDSA signature for message hash m with nonce k and private key d.
   Returns [r, s] or nil."
   ecdsa_forge_with_known_k(m, k, G, d, a, p, curve_n)
}

fn ecdsa_verify(any m, list sig, list G, any Q, any a, any p, any curve_n) bool {
   "Verify ECDSA signature [r,s] for message hash m.
   G: base point. Q: public key = d*G. a,p: curve params. curve_n: order.
   Returns true if valid, false otherwise."
   def r, s = sig[0], sig[1]
   if r <= 0 || r >= curve_n { return false }
   if s <= 0 || s >= curve_n { return false }
   def s_inv = inverse_mod(s, curve_n)
   def u1 = m * s_inv % curve_n
   def u2 = r * s_inv % curve_n
   def P1 = ecc_scalar_mult(u1, G, a, p)
   def P2 = ecc_scalar_mult(u2, Q, a, p)
   def R = ecc_point_add(P1, P2, a, p)
   if R == nil { return false }
   R[0] % curve_n == r
}

fn ecdsa_private_key_from_nonce(list sig, any msg, any curve_n, any nonce) any {
   "Recover an ECDSA private key from one signature when the exact nonce is known."
   def rs = _ecdsa_sig_rs(sig, curve_n)
   if rs == nil { return nil }
   def r, s = rs[0], rs[1]
   ((s * nonce - msg) % curve_n + curve_n) % curve_n * inverse_mod(r, curve_n) % curve_n
}

fn ecdsa_nonce_from_private_key(list sig, any msg, any curve_n, any priv) any {
   "Recover the ECDSA nonce used by a signature when the private key is known."
   def rs = _ecdsa_sig_rs(sig, curve_n)
   if rs == nil { return nil }
   def r, s = rs[0], rs[1]
   ((msg + r * priv) % curve_n + curve_n) % curve_n * inverse_mod(s, curve_n) % curve_n
}

fn _ecdsa_pos_mod(any x, any n) any { ((Z(x) % n) + n) % n }

fn _ecdsa_sig_rs(list sig, any curve_n) any {
   def r, s = _ecdsa_pos_mod(sig[0], curve_n), _ecdsa_pos_mod(sig[1], curve_n)
   (r == 0 || s == 0) ? nil : [r, s]
}

fn _ecdsa_hex_clean(str hex) str {
   mut h = str.strip(hex)
   if str.startswith(h, "0x") || str.startswith(h, "0X") { h = slice(h, 2, h.len) }
   h
}

fn _ecdsa_hex_to_z(str hex) any { hex_to_bigint(_ecdsa_hex_clean(hex)) }

fn ecdsa_hnp_parse_signature_hex(str signature_hex, any baselen=nil) any {
   "Parse a concatenated ECDSA r||s hex signature into [r,s].
   `baselen` is the byte length of r and s ; nil splits the hex string in half."
   def h = _ecdsa_hex_clean(signature_hex)
   mut half = 0
   if baselen == nil {
      if h.len % 2 != 0 { return nil }
      half = h.len / 2
   } else {
      half = int(baselen) * 2
      if h.len < half * 2 { return nil }
   }
   [_ecdsa_hex_to_z(slice(h, 0, half)), _ecdsa_hex_to_z(slice(h, half, half * 2))]
}

fn ecdsa_hnp_parse_line(str line, any baselen=nil) any {
   "Parse a compact HNP signature line: '<hash_hex> <r||s hex>'. Returns [hash, [r,s]]."
   def parts = str.split(str.strip(line), " ")
   if parts.len < 2 { return nil }
   def sig = ecdsa_hnp_parse_signature_hex(parts[1], baselen)
   if sig == nil { return nil }
   [_ecdsa_hex_to_z(parts[0]), sig]
}

fn ecdsa_hnp_sample_residual(list sample, any priv, any curve_n) any {
   "Return t*priv-a modulo curve_n for an ECDSA-derived HNP sample."
   ((Z(sample[0]) * Z(priv) - Z(sample[1])) % curve_n + curve_n) % curve_n
}

fn ecdsa_hnp_residuals(list samples, any priv, any curve_n) list {
   "Return all HNP residuals t*priv-a modulo curve_n for a sample list."
   mut out = []
   mut i = 0
   while i < samples.len {
      out = out.append(ecdsa_hnp_sample_residual(samples[i], priv, curve_n))
      i += 1
   }
   out
}

fn ecdsa_hnp_residual_width(any curve_n, any leaked_bits) any {
   "Return the positive residual width q / 2^leaked_bits, rounded up."
   def denom = Z(1) << int(leaked_bits)
   (Z(curve_n) + denom - Z(1)) / denom
}

fn ecdsa_hnp_count_valid_samples(list samples, any priv, any curve_n, any leaked_bits) int {
   "Count samples whose residual is inside the nonce-leak interval."
   def width = ecdsa_hnp_residual_width(curve_n, leaked_bits)
   mut ok = 0
   mut i = 0
   while i < samples.len {
      def r = ecdsa_hnp_sample_residual(samples[i], priv, curve_n)
      if r >= 0 && r < width { ok += 1 }
      i += 1
   }
   ok
}

fn ecdsa_hnp_check_samples(list samples, any priv, any curve_n, any leaked_bits, int max_errors=0) bool {
   "Validate ECDSA HNP samples against a candidate private key and leakage width."
   def valid = ecdsa_hnp_count_valid_samples(samples, priv, curve_n, leaked_bits)
   samples.len - valid <= int(max_errors)
}

fn ecdsa_hnp_lsb_sample(list sig, any msg, any leaked_lsb, any leaked_bits, any curve_n) any {
   "Convert one ECDSA signature with leaked nonce LSBs into an HNP [t,a] sample.
   The resulting relation is t*d - a = (k - leaked_lsb) / 2^leaked_bits(mod n)."
   def rs = _ecdsa_sig_rs(sig, curve_n)
   if rs == nil { return nil }
   def r, s = rs[0], rs[1]
   def scale = inverse_mod(Z(1) << int(leaked_bits), curve_n)
   def s_inv = inverse_mod(s, curve_n)
   def t = scale * s_inv % curve_n * r % curve_n
   def a = scale * ((Z(leaked_lsb) - s_inv * Z(msg)) % curve_n) % curve_n
   [t, a]
}

fn ecdsa_hnp_msb_sample(list sig, any msg, any leaked_msb, any leaked_bits, any curve_n, any nonce_bits=nil) any {
   "Convert one ECDSA signature with leaked nonce MSBs into an HNP [t,a] sample.
   `nonce_bits` defaults to bit_length(n-1)."
   def rs = _ecdsa_sig_rs(sig, curve_n)
   if rs == nil { return nil }
   def r, s = rs[0], rs[1]
   def bits = nonce_bits == nil ? bit_length(curve_n - Z(1)) : int(nonce_bits)
   def shift = bits - int(leaked_bits)
   if shift < 0 { return nil }
   def s_inv = inverse_mod(s, curve_n)
   def t = s_inv * r % curve_n
   def a = (Z(leaked_msb) * (Z(1) << shift) - s_inv * Z(msg)) % curve_n
   [t, a]
}

fn _ecdsa_hnp_sample(list sig, any msg, any leak, any leaked_bits, any curve_n, str mode, any nonce_bits=nil) any {
   if mode == "msb" { return ecdsa_hnp_msb_sample(sig, msg, leak, leaked_bits, curve_n, nonce_bits) }
   ecdsa_hnp_lsb_sample(sig, msg, leak, leaked_bits, curve_n)
}

fn _ecdsa_hnp_line(str line, any leak, any leaked_bits, any curve_n, any baselen, str mode, any nonce_bits=nil) any {
   def parsed = ecdsa_hnp_parse_line(line, baselen)
   parsed == nil ? nil : _ecdsa_hnp_sample(parsed[1], parsed[0], leak, leaked_bits, curve_n, mode, nonce_bits)
}

fn _ecdsa_hnp_samples(list sigs, list msgs, list leaks, any leaked_bits, any curve_n, str mode, any nonce_bits=nil) any {
   mut out = []
   mut i = 0
   while i < sigs.len && i < msgs.len && i < leaks.len {
      def sample = _ecdsa_hnp_sample(sigs[i], msgs[i], leaks[i], leaked_bits, curve_n, mode, nonce_bits)
      if sample == nil { return nil }
      out = out.append(sample)
      i += 1
   }
   out
}

fn _ecdsa_hnp_lines(list lines, list leaks, any leaked_bits, any curve_n, any baselen, str mode, any nonce_bits=nil) any {
   mut out = []
   mut i = 0
   while i < lines.len && i < leaks.len {
      def sample = _ecdsa_hnp_line(lines[i], leaks[i], leaked_bits, curve_n, baselen, mode, nonce_bits)
      if sample == nil { return nil }
      out = out.append(sample)
      i += 1
   }
   out
}

fn ecdsa_hnp_lsb_line(str line, any leaked_lsb, any leaked_bits, any curve_n, any baselen=nil) any {
   "Convert one '<hash_hex> <r||s hex>' line plus leaked nonce LSBs into an HNP sample."
   _ecdsa_hnp_line(line, leaked_lsb, leaked_bits, curve_n, baselen, "lsb")
}

fn ecdsa_hnp_msb_line(str line, any leaked_msb, any leaked_bits, any curve_n, any baselen=nil, any nonce_bits=nil) any {
   "Convert one '<hash_hex> <r||s hex>' line plus leaked nonce MSBs into an HNP sample."
   _ecdsa_hnp_line(line, leaked_msb, leaked_bits, curve_n, baselen, "msb", nonce_bits)
}

fn ecdsa_hnp_lsb_samples(list sigs, list msgs, list leaked_lsbs, any leaked_bits, any curve_n) any {
   "Convert lists of ECDSA signatures, hashes, and nonce LSB leaks into HNP samples."
   _ecdsa_hnp_samples(sigs, msgs, leaked_lsbs, leaked_bits, curve_n, "lsb")
}

fn ecdsa_hnp_lsb_lines(list lines, list leaked_lsbs, any leaked_bits, any curve_n, any baselen=nil) any {
   "Convert compact signature lines and LSB leaks into HNP samples."
   _ecdsa_hnp_lines(lines, leaked_lsbs, leaked_bits, curve_n, baselen, "lsb")
}

fn ecdsa_hnp_msb_lines(list lines, list leaked_msbs, any leaked_bits, any curve_n, any baselen=nil, any nonce_bits=nil) any {
   "Convert compact signature lines and MSB leaks into HNP samples."
   _ecdsa_hnp_lines(lines, leaked_msbs, leaked_bits, curve_n, baselen, "msb", nonce_bits)
}

fn ecdsa_hnp_check_lsb_lines(list lines, list leaked_lsbs, any leaked_bits, any curve_n, any priv, any baselen=nil, int max_errors=0) bool {
   "Parse and validate compact LSB line/leak datasets for a candidate key."
   def samples = ecdsa_hnp_lsb_lines(lines, leaked_lsbs, leaked_bits, curve_n, baselen)
   if samples == nil { return false }
   ecdsa_hnp_check_samples(samples, priv, curve_n, leaked_bits, max_errors)
}

fn ecdsa_hnp_check_msb_lines(list lines, list leaked_msbs, any leaked_bits, any curve_n, any priv, any baselen=nil, any nonce_bits=nil, int max_errors=0) bool {
   "Parse and validate compact MSB line/leak datasets for a candidate key."
   def samples = ecdsa_hnp_msb_lines(lines, leaked_msbs, leaked_bits, curve_n, baselen, nonce_bits)
   if samples == nil { return false }
   ecdsa_hnp_check_samples(samples, priv, curve_n, leaked_bits, max_errors)
}

fn ecdsa_hnp_msb_samples(list sigs, list msgs, list leaked_msbs, any leaked_bits, any curve_n, any nonce_bits=nil) any {
   "Convert lists of ECDSA signatures, hashes, and nonce MSB leaks into HNP samples."
   _ecdsa_hnp_samples(sigs, msgs, leaked_msbs, leaked_bits, curve_n, "msb", nonce_bits)
}

fn _ecdsa_hnp_fail(str reason, str mode, int sample_count=0) dict {
   mut out = dict(8)
   out = out.set("ok", false)
   out = out.set("key", nil)
   out = out.set("alpha", nil)
   out = out.set("reason", reason)
   out = out.set("mode", mode)
   out = out.set("samples", sample_count)
   out
}

fn ecdsa_hnp_recover(list samples, any curve_n, any leaked_bits, any opts=nil) any {
   "Recover an ECDSA private key from already-built HNP [t,a] samples.
   Returns the HNP result dict ; recovered key is available as `key`."
   if samples == nil { return _ecdsa_hnp_fail("invalid samples", "samples") }
   def res = hnp.hnp_recover(samples, curve_n, leaked_bits, opts)
   is_dict(res) ? res.set("mode", "samples") : _ecdsa_hnp_fail("hnp recovery failed", "samples", samples.len)
}

fn ecdsa_recover_key_from_lsb_lines(list lines, list leaked_lsbs, any leaked_bits, any curve_n, any baselen=nil, any opts=nil) any {
   "Recover an ECDSA private key from '<hash_hex> <r||s hex>' lines and nonce LSB leaks."
   def samples = ecdsa_hnp_lsb_lines(lines, leaked_lsbs, leaked_bits, curve_n, baselen)
   if samples == nil { return _ecdsa_hnp_fail("invalid lsb dataset", "lsb") }
   def res = hnp.hnp_recover(samples, curve_n, leaked_bits, opts)
   is_dict(res) ? res.set("mode", "lsb") : _ecdsa_hnp_fail("hnp recovery failed", "lsb", samples.len)
}

fn ecdsa_recover_key_from_msb_lines(list lines, list leaked_msbs, any leaked_bits, any curve_n, any baselen=nil, any nonce_bits=nil, any opts=nil) any {
   "Recover an ECDSA private key from '<hash_hex> <r||s hex>' lines and nonce MSB leaks."
   def samples = ecdsa_hnp_msb_lines(lines, leaked_msbs, leaked_bits, curve_n, baselen, nonce_bits)
   if samples == nil { return _ecdsa_hnp_fail("invalid msb dataset", "msb") }
   def res = hnp.hnp_recover(samples, curve_n, leaked_bits, opts)
   is_dict(res) ? res.set("mode", "msb") : _ecdsa_hnp_fail("hnp recovery failed", "msb", samples.len)
}

fn ecdsa_partial_nonce_leak(list sig, any msg, any curve_n, any k_high_bits, int unknown_bits) any {
   "Recover ECDSA private key d when high bits of nonce k are known.
   Searches 2^unknown_bits candidates. Practical when unknown_bits < 16.
   sig: [r,s]. msg: hash. k_high_bits: known high portion. unknown_bits: missing bits.
   Returns d or nil."
   def r, s = sig[0], sig[1]
   def k_base = k_high_bits << unknown_bits
   def r_inv = inverse_mod(r, curve_n)
   def limit = 1 << unknown_bits
   mut lo = 0
   while lo < limit {
      def k, d = k_base + lo, ((s * k - msg) % curve_n + curve_n) % curve_n * r_inv % curve_n
      def k_check = (msg + r * d) % curve_n * inverse_mod(s, curve_n) % curve_n
      if k_check == k { return d }
      lo += 1
   }
   nil
}

fn _ecdsa_key_matches_public(any d, any G, any Q, any a, any p) bool {
   if G == nil || Q == nil || a == nil || p == nil { return true }
   ecc_scalar_mult(d, G, a, p) == Q
}

fn ecdsa_recover_key_from_nonce_lsb(list sig, any msg, any n, any leak, any bits, any hi_bits, any G=nil, any Q=nil, any a=nil, any p=nil) any {
   "Brute-force a private key when nonce LSBs are known and few high bits are unknown.
   Provide G/Q/a/p to reject false keys against the public key."
   def low_mask = Z(1) << int(bits)
   def limit = Z(1) << int(hi_bits)
   mut hi = Z(0)
   while hi < limit {
      def k = hi * low_mask + Z(leak)
      if k > 0 && k < n {
         def d = ecdsa_private_key_from_nonce(sig, msg, n, k)
         if d != nil && _ecdsa_key_matches_public(d, G, Q, a, p) { return d }
      }
      hi += Z(1)
   }
   nil
}

fn ecdsa_recover_key_from_nonce_msb(list sig, any msg, any n, any leak, any bits, any lo_bits, any G=nil, any Q=nil, any a=nil, any p=nil) any {
   "Brute-force a private key when nonce MSBs are known and few low bits are unknown.
   Provide G/Q/a/p to reject false keys against the public key."
   def low_limit = Z(1) << int(lo_bits)
   def high = Z(leak) << int(lo_bits)
   mut lo = Z(0)
   while lo < low_limit {
      def k = high + lo
      if k > 0 && k < n {
         def d = ecdsa_private_key_from_nonce(sig, msg, n, k)
         if d != nil && _ecdsa_key_matches_public(d, G, Q, a, p) { return d }
      }
      lo += Z(1)
   }
   nil
}

#main {
   def curve = ecc_curve_p256()
   def ecc_p256_p = curve[0]
   def ecc_p256_a = curve[1]
   def ecc_p256_b = curve[2]
   def ecc_p256_G = curve[3]
   def ecc_p256_n = curve[4]
   def ecc_p256_gx = ecc_p256_G[0]
   def ecc_p256_gy = ecc_p256_G[1]
   def h = Z(12345)
   def d = Z(67890)
   def k = Z(98765)
   def sig = ecdsa_sign(h, k, ecc_p256_G, d, ecc_p256_a, ecc_p256_p, ecc_p256_n)
   assert(sig != nil, "ecdsa sign returns sig")
   def Q = ecc_scalar_mult(d, ecc_p256_G, ecc_p256_a, ecc_p256_p)
   assert(ecdsa_verify(h, sig, ecc_p256_G, Q, ecc_p256_a, ecc_p256_p, ecc_p256_n), "ecdsa verify passes")
   def pk = ecdsa_private_key_from_nonce(sig, h, ecc_p256_n, k)
   assert(pk == d, "ecdsa private key from nonce")
   def parsed = ecdsa_hnp_parse_line("0xdeadbeef cafebabedeadbeef")
   assert(parsed != nil, "hnp parse line succeeds")
   assert(parsed[0] == Z(3735928559), "hnp parse line hash")
   assert(parsed[1][0] == Z(3405691582), "hnp parse line r")
   assert(parsed[1][1] == Z(3735928559), "hnp parse line s")
   def leaked_bits = 8
   def leaked_lsb = k % (Z(1) << leaked_bits)
   def sample = ecdsa_hnp_lsb_sample(sig, h, leaked_lsb, leaked_bits, ecc_p256_n)
   assert(sample != nil, "hnp lsb sample built")
   def residual = ecdsa_hnp_sample_residual(sample, d, ecc_p256_n)
   def expected_residual = ((k - leaked_lsb) * inverse_mod(Z(1) << leaked_bits, ecc_p256_n)) % ecc_p256_n
   assert(residual == ((expected_residual % ecc_p256_n) + ecc_p256_n) % ecc_p256_n, "hnp residual matches expected relation")
   assert(ecdsa_hnp_check_samples([sample], d, ecc_p256_n, leaked_bits) == true, "hnp check samples")
   def samples = ecdsa_hnp_lsb_samples([sig], [h], [leaked_lsb], leaked_bits, ecc_p256_n)
   assert(samples.len == 1, "hnp lsb samples count")
   assert(samples[0] == sample, "hnp lsb samples matches single sample")
   def h3 = Z(54321)
   def sig3 = ecdsa_sign(h3, k, ecc_p256_G, d, ecc_p256_a, ecc_p256_p, ecc_p256_n)
   def reused = ecdsa_nonce_reuse(sig, sig3, h, h3, ecc_p256_n)
   assert(reused != nil, "nonce reuse detection")
   assert(reused[0] == k, "nonce reuse recovers k")
   assert(reused[1] == d, "nonce reuse recovers d")
   def k2 = Z(123456)
   def sig2 = ecdsa_sign(h, k2, ecc_p256_G, d, ecc_p256_a, ecc_p256_p, ecc_p256_n)
   assert(sig2 != nil, "second ecdsa sign")
   def recovered = ecdsa_recover_key_from_two_sigs(sig[0], sig[1], h, sig2[0], sig2[1], h, ecc_p256_n)
   assert(recovered == nil, "nonce reuse with distinct ks returns nil")
   assert(ecdsa_forge_with_known_k(h, k, ecc_p256_G, d, ecc_p256_a, ecc_p256_p, ecc_p256_n) == sig, "forge with known k reproduces signature")
   print("✓ std.math.crypto.ecc.ecdsa self-test passed")
}
