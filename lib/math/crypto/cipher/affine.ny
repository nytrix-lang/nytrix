;; Keywords: cipher affine math crypto
;; Affine cipher encryption, decryption, and recovery routines.
;; Reference:
;; - https://netlab.cs.ucla.edu/wiki/files/shannon1949.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap1.pdf
;; References:
;; - std.math.crypto.cipher
;; - std.math.crypto.analysis
module std.math.crypto.cipher.affine(affine_encrypt, affine_decrypt, affine_decrypt_bytes,
   affine_crack_bytes_contains, affine_crack_bytes_known_substring,
   affine_encrypt_alphabet, affine_decrypt_alphabet, affine_decrypt_block_pairs_alphabet,
   affine_cbc_decrypt_alphabet, affine_score_ngrams, affine_cbc_crack_alphabet,
   affine_crack_block_pairs_alphabet, affine_cbc_crack_known_prefix_alphabet,
   affine_crack_block_pairs_known_prefix_alphabet, affine_crack_known_pt_alphabet, affine_crack_known_pt)
use std.core
use std.math.nt
use std.core.str
use std.math.crypto.error
use std.math.crypto.support.tools as support

fn affine_mod_inverse(number a, number m) number {
   "Compute the modular multiplicative inverse of a mod m. Returns the inverse or 0 if it does not exist."
   def eg = extended_gcd(a, m)
   def g = eg[0]
   def x = eg[1]
   if !bigint_eq(g, 1) { return 0 }
   def result = (x % m + m) % m
   result
}

fn _affine_index(str alphabet, str ch) int {
   find(alphabet, ch)
}

fn _affine_pair_value(str alphabet, str s, int i) int {
   def m = alphabet.len
   def x0 = _affine_index(alphabet, utf8_slice(s, i, i + 1, 1))
   def x1 = _affine_index(alphabet, utf8_slice(s, i + 1, i + 2, 1))
   if x0 < 0 || x1 < 0 { return -1 }
   x0 * m + x1
}

fn _affine_builder_take(list b) str {
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn _affine_map_value(number value, number a, number b, number m, bool decrypt) number {
   if decrypt { return (a * ((value - b + m) % m)) % m }
   (a * value + b) % m
}

fn _affine_map_ascii_upper(str text, number a, number b, number m, bool decrypt) str {
   mut result = Builder(text.len + 8)
   mut i = 0
   while i < text.len {
      def c = load8(text, i)
      case c {
         65..90 -> { result = builder_append(result, chr(_affine_map_value(c - 65, a, b, m, decrypt) + 65)) }
         _ -> { result = builder_append(result, chr(c)) }
      }
      i += 1
   }
   _affine_builder_take(result)
}

fn _affine_map_alphabet(str text, number a, number b, str alphabet, bool decrypt) str {
   def m = alphabet.len
   mut result = Builder(text.len + 8)
   mut i = 0
   while i < text.len {
      def ch = utf8_slice(text, i, i + 1, 1)
      def pos = _affine_index(alphabet, ch)
      if pos >= 0 {
         def mapped = _affine_map_value(pos, a, b, m, decrypt)
         result = builder_append(result, utf8_slice(alphabet, mapped, mapped + 1, 1))
      } else {
         result = builder_append(result, ch)
      }
      i += 1
   }
   _affine_builder_take(result)
}

fn affine_encrypt(str msg, number a, number b, number m=26) str {
   "Encrypt a message using the Affine cipher: y = (a * x + b) mod m for each letter.
   msg: uppercase alphabetic string to encrypt
   a: multiplicative key(must be coprime with m)
   b: additive key
   m: alphabet size(default 26)
   Returns the encrypted uppercase string."
   _affine_map_ascii_upper(msg, a, b, m, false)
}

fn affine_encrypt_alphabet(str msg, number a, number b, str alphabet) str {
   "Encrypt using an affine map over a custom alphabet string."
   crypto_require(msg != nil, "cipher.affine_encrypt_alphabet", "msg is nil")
   crypto_require_nonempty(alphabet, "cipher.affine_encrypt_alphabet", "alphabet")
   _affine_map_alphabet(msg, a, b, alphabet, false)
}

fn affine_decrypt(str ct, number a, number b, number m=26) any {
   "Decrypt a ciphertext encrypted with the Affine cipher using x = a_inv * (y - b) mod m.
   ct: uppercase alphabetic string to decrypt
   a: multiplicative key used during encryption
   b: additive key used during encryption
   m: alphabet size(default 26)
   Returns the decrypted uppercase string."
   def a_inv = affine_mod_inverse(a, m)
   a_inv == 0 ? nil : _affine_map_ascii_upper(ct, a_inv, b, m, true)
}

fn affine_decrypt_bytes(list ct, number a, number b, number m=256) ?list {
   "Decrypt byte-list affine ciphertext using x = a^-1*(y-b) mod m."
   def a_inv = affine_mod_inverse(a, m)
   if a_inv == 0 { return nil }
   mut result = []
   mut i = 0
   while i < ct.len {
      def y = ct[i]
      result = result.append(int((a_inv * ((y - b + m) % m)) % m))
      i += 1
   }
   result
}

fn affine_crack_bytes_contains(list ct, list needle, number m=256) any {
   "Brute-force byte affine keys and return [a, b, plaintext_bytes] when plaintext contains needle."
   mut a = 1
   while a < m {
      if gcd(a, m) == 1 {
         mut b = 0
         while b < m {
            def pt = affine_decrypt_bytes(ct, a, b, m)
            if pt != nil && support.bytes_contains(pt, needle) { return [a, b, pt] }
            b += 1
         }
      }
      a += 1
   }
   nil
}

fn affine_crack_bytes_known_substring(list ct, list needle, number m=256) any {
   "Recover byte-affine keys by sliding a known plaintext substring over ciphertext. Returns [a, b, offset, plaintext_bytes]."
   if needle.len < 2 || needle.len > ct.len { return nil }
   def x0, x1 = needle[0], needle[1]
   def dx = (x1 - x0 + m) % m
   def dx_inv = affine_mod_inverse(dx, m)
   if dx_inv == 0 { return nil }
   mut off = 0
   while off <= ct.len - needle.len {
      def y0, y1 = ct[off], ct[off + 1]
      def a = (((y1 - y0 + m) % m) * dx_inv) % m
      if gcd(a, m) == 1 {
         def b = ((y0 - a * x0) % m + m) % m
         mut ok = true
         mut i = 0
         while i < needle.len {
            if ((a * needle[i] + b) % m) != ct[off + i] { ok = false }
            i += 1
         }
         if ok {
            def pt = affine_decrypt_bytes(ct, a, b, m)
            if pt != nil { return [a, b, off, pt] }
         }
      }
      off += 1
   }
   nil
}

fn affine_decrypt_alphabet(str ct, number a, number b, str alphabet) any {
   "Decrypt an affine ciphertext over a custom alphabet string."
   crypto_require(ct != nil, "cipher.affine_decrypt_alphabet", "ct is nil")
   crypto_require_nonempty(alphabet, "cipher.affine_decrypt_alphabet", "alphabet")
   def m = alphabet.len
   def a_inv = affine_mod_inverse(a, m)
   if a_inv == 0 { return nil }
   _affine_map_alphabet(ct, a_inv, b, alphabet, true)
}

fn affine_decrypt_block_pairs_alphabet(str ct, number a, number b, str alphabet) any {
   "Decrypt affine ciphertext encoded over pairs of symbols from a custom alphabet.
   Each ciphertext digraph is treated as a value in base-|alphabet| and inverted mod |alphabet|^2."
   crypto_require(ct != nil, "cipher.affine_decrypt_block_pairs_alphabet", "ct is nil")
   crypto_require_nonempty(alphabet, "cipher.affine_decrypt_block_pairs_alphabet", "alphabet")
   def m = alphabet.len
   def mod = m * m
   def a_inv = affine_mod_inverse(a, mod)
   if a_inv == 0 { return nil }
   mut result = ""
   mut i = 0
   while i + 1 < ct.len {
      def y = _affine_pair_value(alphabet, ct, i)
      if y < 0 { return nil }
      def x = (a_inv * ((y - b + mod) % mod)) % mod
      def p0 = x / m
      def p1 = x % m
      result = str_add(result, utf8_slice(alphabet, p0, p0 + 1, 1))
      result = str_add(result, utf8_slice(alphabet, p1, p1 + 1, 1))
      i += 2
   }
   result
}

fn affine_cbc_decrypt_alphabet(str ct, number a, number b, str iv_char, str alphabet) any {
   "Decrypt affine-CBC over a custom alphabet.
   The affine inverse is applied first, then the previous ciphertext symbol is removed mod alphabet size."
   crypto_require(ct != nil, "cipher.affine_cbc_decrypt_alphabet", "ct is nil")
   crypto_require_nonempty(alphabet, "cipher.affine_cbc_decrypt_alphabet", "alphabet")
   crypto_require_nonempty(iv_char, "cipher.affine_cbc_decrypt_alphabet", "iv_char")
   def m = alphabet.len
   def a_inv = affine_mod_inverse(a, m)
   if a_inv == 0 { return nil }
   def iv = _affine_index(alphabet, iv_char)
   if iv < 0 { return nil }
   mut prev = iv
   mut result = ""
   mut i = 0
   while i < ct.len {
      def ch = utf8_slice(ct, i, i + 1, 1)
      def pos = _affine_index(alphabet, ch)
      if pos >= 0 {
         def y, p = (a_inv * ((pos - b + m) % m)) % m, (y - prev + m) % m
         result = str_add(result, utf8_slice(alphabet, p, p + 1, 1))
         prev = pos
      } else {
         result = str_add(result, ch)
      }
      i += 1
   }
   result
}

fn _affine_has_at(str s, str needle, int pos) bool {
   if pos < 0 || pos + needle.len > s.len { return false }
   utf8_slice(s, pos, pos + needle.len, 1) == needle
}

fn _affine_count_sub(str s, str needle) int {
   mut hits = 0
   mut i = 0
   while i + needle.len <= s.len {
      if _affine_has_at(s, needle, i) { hits += 1 }
      i += 1
   }
   hits
}

fn _affine_rows_match(list xs, list ys, number a, number b, number m, int n) bool {
   mut k = 0
   while k < n {
      if (a * xs[k] + b) % m != ys[k] { return false }
      k += 1
   }
   true
}

fn _affine_solve_linear_rows(list xs, list ys, number m) any {
   def n = min(xs.len, ys.len)
   mut r = 0
   while r < n {
      mut s = r + 1
      while s < n {
         def dx, dy = (xs[s] - xs[r] + m) % m, (ys[s] - ys[r] + m) % m
         def dx_inv = affine_mod_inverse(dx, m)
         if dx_inv != 0 {
            def a, b = (dy * dx_inv) % m, ((ys[r] - a * xs[r]) % m + m) % m
            if _affine_rows_match(xs, ys, a, b, m, n) { return [a, b] }
         }
         s += 1
      }
      r += 1
   }
   nil
}

fn affine_score_ngrams(str s, list ngrams) int {
   "Score text by counting expected substrings. Longer substrings receive slightly more weight."
   crypto_require(s != nil, "cipher.affine_score_ngrams", "s is nil")
   crypto_require(ngrams != nil, "cipher.affine_score_ngrams", "ngrams is nil")
   mut score = 0
   mut i = 0
   while i < ngrams.len {
      def needle = ngrams[i]
      if needle.len > 0 { score += _affine_count_sub(s, needle) * max(1, needle.len) }
      i += 1
   }
   score
}

fn _affine_should_stop(int score, any stop_score) bool {
   stop_score != nil && score >= stop_score
}

fn _affine_cbc_scored_candidate(str ct, str prefix, number a, number b, str iv_char, str alphabet, list ngrams, int min_score) any {
   def head = affine_cbc_decrypt_alphabet(prefix, a, b, iv_char, alphabet)
   if head == nil || affine_score_ngrams(head, ngrams) < min_score { return nil }
   def pt = affine_cbc_decrypt_alphabet(ct, a, b, iv_char, alphabet)
   if pt == nil { return nil }
   def score = affine_score_ngrams(pt, ngrams)
   [a, b, iv_char, pt, score]
}

fn _affine_pair_scored_candidate(str ct, str prefix, number a, number b, str alphabet, list ngrams, int min_score) any {
   def head = affine_decrypt_block_pairs_alphabet(prefix, a, b, alphabet)
   if head == nil || affine_score_ngrams(head, ngrams) < min_score { return nil }
   def pt = affine_decrypt_block_pairs_alphabet(ct, a, b, alphabet)
   if pt == nil { return nil }
   def score = affine_score_ngrams(pt, ngrams)
   [a, b, pt, score]
}

fn affine_cbc_crack_alphabet(str ct, str alphabet, list ngrams, int min_score=1, int prefix_len=180, any stop_score=nil) any {
   "Brute-force affine-CBC parameters over a custom alphabet using substring scoring.
   Returns [a, b, iv_char, plaintext, score], or nil when no candidate reaches min_score."
   crypto_require_nonempty(ct, "cipher.affine_cbc_crack_alphabet", "ct")
   crypto_require_nonempty(alphabet, "cipher.affine_cbc_crack_alphabet", "alphabet")
   def m = alphabet.len
   def prefix = utf8_slice(ct, 0, min(ct.len, prefix_len), 1)
   mut best = nil
   mut best_score = min_score - 1
   mut a = 1
   while a < m {
      if gcd(a, m) == 1 {
         mut b = 0
         while b < m {
            mut iv = 0
            while iv < m {
               def iv_char = utf8_slice(alphabet, iv, iv + 1, 1)
               def cand = _affine_cbc_scored_candidate(ct, prefix, a, b, iv_char, alphabet, ngrams, min_score)
               if cand != nil {
                  def score = cand.get(4)
                  if score > best_score {
                     best_score = score
                     best = cand
                     if _affine_should_stop(score, stop_score) { return best }
                  }
               }
               iv += 1
            }
            b += 1
         }
      }
      a += 1
   }
   best
}

fn affine_crack_block_pairs_alphabet(str ct, str alphabet, list ngrams, int min_score=1, int prefix_len=160, any stop_score=nil) any {
   "Brute-force affine parameters for pair/block alphabet encoding.
   Returns [a, b, plaintext, score], or nil when no candidate reaches min_score."
   crypto_require_nonempty(ct, "cipher.affine_crack_block_pairs_alphabet", "ct")
   crypto_require_nonempty(alphabet, "cipher.affine_crack_block_pairs_alphabet", "alphabet")
   def m = alphabet.len
   def mod = m * m
   def prefix = utf8_slice(ct, 0, min(ct.len, prefix_len), 1)
   mut best = nil
   mut best_score = min_score - 1
   mut a = 1
   while a < mod {
      if gcd(a, mod) == 1 {
         mut b = 0
         while b < mod {
            def cand = _affine_pair_scored_candidate(ct, prefix, a, b, alphabet, ngrams, min_score)
            if cand != nil {
               def score = cand.get(3)
               if score > best_score {
                  best_score = score
                  best = cand
                  if _affine_should_stop(score, stop_score) { return best }
               }
            }
            b += 1
         }
      }
      a += 1
   }
   best
}

fn _affine_cbc_prefix_rows(str ct, str pt_prefix, str alphabet, int iv, int n, int m) any {
   mut xs, ys = [], []
   mut prev = iv
   mut i = 0
   while i < n {
      def p = _affine_index(alphabet, utf8_slice(pt_prefix, i, i + 1, 1))
      def c = _affine_index(alphabet, utf8_slice(ct, i, i + 1, 1))
      if p < 0 || c < 0 { return nil }
      xs, ys = xs.append((p + prev) % m), ys.append(c)
      prev = c
      i += 1
   }
   [xs, ys]
}

fn affine_cbc_crack_known_prefix_alphabet(str ct, str pt_prefix, str alphabet) any {
   "Recover affine-CBC parameters from a known plaintext prefix.
   Returns [a, b, iv_char, plaintext], or nil when the prefix is inconsistent."
   crypto_require_nonempty(ct, "cipher.affine_cbc_crack_known_prefix_alphabet", "ct")
   crypto_require_nonempty(pt_prefix, "cipher.affine_cbc_crack_known_prefix_alphabet", "pt_prefix")
   crypto_require_nonempty(alphabet, "cipher.affine_cbc_crack_known_prefix_alphabet", "alphabet")
   def m, n = alphabet.len, min(ct.len, pt_prefix.len)
   mut iv = 0
   while iv < m {
      def rows = _affine_cbc_prefix_rows(ct, pt_prefix, alphabet, iv, n, m)
      if rows != nil {
         def keys = _affine_solve_linear_rows(rows[0], rows[1], m)
         if keys != nil {
            def a, b = keys[0], keys[1]
            def iv_char = utf8_slice(alphabet, iv, iv + 1, 1)
            def pt = affine_cbc_decrypt_alphabet(ct, a, b, iv_char, alphabet)
            return [a, b, iv_char, pt]
         }
      }
      iv += 1
   }
   nil
}

fn affine_crack_block_pairs_known_prefix_alphabet(str ct, str pt_prefix, str alphabet) any {
   "Recover affine pair/block parameters from a known plaintext prefix.
   Returns [a, b, plaintext], or nil when the prefix is inconsistent."
   crypto_require_nonempty(ct, "cipher.affine_crack_block_pairs_known_prefix_alphabet", "ct")
   crypto_require_nonempty(pt_prefix, "cipher.affine_crack_block_pairs_known_prefix_alphabet", "pt_prefix")
   crypto_require_nonempty(alphabet, "cipher.affine_crack_block_pairs_known_prefix_alphabet", "alphabet")
   def m = alphabet.len
   def mod = m * m
   def n = min(ct.len, pt_prefix.len)
   def pairs = n / 2
   mut xs, ys = [], []
   mut i = 0
   while i < pairs {
      def p, c = _affine_pair_value(alphabet, pt_prefix, i * 2), _affine_pair_value(alphabet, ct, i * 2)
      if p < 0 || c < 0 { return nil }
      xs, ys = xs.append(p), ys.append(c)
      i += 1
   }
   def keys = _affine_solve_linear_rows(xs, ys, mod)
   if keys != nil {
      def a, b = keys[0], keys[1]
      def pt = affine_decrypt_block_pairs_alphabet(ct, a, b, alphabet)
      return [a, b, pt]
   }
   nil
}

fn affine_crack_known_pt_alphabet(str ct, str pt, str alphabet) any {
   "Recover affine keys [a,b] for a custom alphabet from a known plaintext prefix/pair."
   crypto_require_nonempty(ct, "cipher.affine_crack_known_pt_alphabet", "ct")
   crypto_require_nonempty(pt, "cipher.affine_crack_known_pt_alphabet", "pt")
   crypto_require_nonempty(alphabet, "cipher.affine_crack_known_pt_alphabet", "alphabet")
   def m, n = alphabet.len, min(ct.len, pt.len)
   mut i = 0
   while i < n {
      mut j = i + 1
      while j < n {
         def xp1 = _affine_index(alphabet, utf8_slice(pt, i, i + 1, 1))
         def xp2 = _affine_index(alphabet, utf8_slice(pt, j, j + 1, 1))
         def yc1 = _affine_index(alphabet, utf8_slice(ct, i, i + 1, 1))
         def yc2 = _affine_index(alphabet, utf8_slice(ct, j, j + 1, 1))
         if xp1 >= 0 && xp2 >= 0 && yc1 >= 0 && yc2 >= 0 {
            def dx, dy = (xp2 - xp1 + m) % m, (yc2 - yc1 + m) % m
            def dx_inv = affine_mod_inverse(dx, m)
            if dx_inv != 0 {
               def a, b = (dy * dx_inv) % m, ((yc1 - a * xp1) % m + m) % m
               if affine_encrypt_alphabet(pt, a, b, alphabet) == utf8_slice(ct, 0, pt.len, 1) { return [a, b] }
            }
         }
         j += 1
      }
      i += 1
   }
   nil
}

fn affine_crack_known_pt(str ct, str pt, number m=26) any {
   "Recover the Affine cipher keys(a, b) from a known plaintext-ciphertext pair.
   ct: ciphertext string(must be at least 2 characters)
   pt: corresponding plaintext string(must be at least 2 characters)
   m: alphabet size(default 26)
   Returns [a, b] as a list, or nil if the keys cannot be determined."
   def ct_len = ct.len
   if ct_len < 2 { return nil }
   def pt_len = pt.len
   if pt_len < 2 { return nil }
   def y1, y2 = load8(ct, 0) - 65, load8(ct, 1) - 65
   def x1, x2 = load8(pt, 0) - 65, load8(pt, 1) - 65
   def dx, dy = (x2 - x1 + m) % m, (y2 - y1 + m) % m
   if dx == 0 { return nil }
   def dx_inv = affine_mod_inverse(dx, m)
   if dx_inv == 0 { return nil }
   def a, b = (dy * dx_inv) % m, ((y1 - a * x1) % m + m) % m
   [a, b]
}

#main {
   def msg = "AFFINE"
   def a = 5
   def b = 8
   def m = 26
   def enc = affine_encrypt(msg, a, b, m)
   def dec = affine_decrypt(enc, a, b, m)
   assert(dec == msg, "affine encrypt decrypt roundtrip")
   def ct = affine_encrypt("HELP", a, b, m)
   def keys = affine_crack_known_pt(ct, "HELP", m)
   assert(keys != nil, "affine crack known plaintext")
   assert(keys[0] == a, "affine crack recovers a")
   assert(keys[1] == b, "affine crack recovers b")
   def bpt = [68, 72, 123, 111, 107, 125]
   mut bct = []
   mut bi = 0
   while bi < bpt.len {
      bct = bct.append((3 * bpt[bi] + 7) % 251)
      bi += 1
   }
   def bhit = affine_crack_bytes_contains(bct, [68, 72, 123], 251)
   assert(bhit != nil, "affine byte crack hit")
   assert(bhit[0] == 3 && bhit[1] == 7, "affine byte crack keys")
   assert(bhit[2] == bpt, "affine byte plaintext")
   def khit = affine_crack_bytes_known_substring(bct, [68, 72], 251)
   assert(khit != nil, "affine byte known substring hit")
   assert(khit[0] == 3 && khit[1] == 7, "affine byte known substring keys")
   assert(khit[3] == bpt, "affine byte known substring plaintext")
   print("✓ std.math.crypto.cipher.affine self-test passed")
}
