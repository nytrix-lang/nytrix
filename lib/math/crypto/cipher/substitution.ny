;; Keywords: cipher substitution math crypto
;; Substitution cipher scoring and key recovery routines.
;; Reference:
;; - https://practicalcryptography.com/ciphers/simple-substitution-cipher/
;; - https://en.wikipedia.org/wiki/Letter_frequency
;; References:
;; - std.math.crypto.cipher
;; - std.math.crypto.analysis
module std.math.crypto.cipher.substitution(substitution_encrypt, substitution_decrypt, substitution_apply_key, substitution_freq_analysis, substitution_key_from_pairs, substitution_freq_key, substitution_crack_freq, substitution_crack_hill, substitution_crack_hill_multistart, substitution_score, substitution_tr)
use std.core
use std.core.str
use std.math.crypto.error
use std.math.crypto.encoding.xor

def ENGLISH_FREQ_ORDER = "ETAOINSHRDLCUMWFGYPBVKJXQZ"

@inline
fn _sub_is_alpha_code(int c) bool { (c >= 65 && c <= 90) || (c >= 97 && c <= 122) }

fn _sub_inverse_key_map(str key) list {
   mut inv = []
   mut i = 0
   while i < 26 {
      inv = inv.append(-1)
      i += 1
   }
   i = 0
   while i < 26 {
      def code = ord(upper(utf8_slice(key, i, i + 1, 1)))
      case code {
         65..90 -> { inv[code - 65] = i }
         _ -> {}
      }
      i += 1
   }
   inv
}

fn _sub_text_score(str text) int {
   mut score = english_score(text.to_bytes) + substitution_score(text)
   def upper_text = " " + upper(text) + " "
   if upper_text.contains(" THE ") { score += 220 }
   if upper_text.contains(" AND ") { score += 160 }
   if upper_text.contains(" THIS ") { score += 160 }
   if upper_text.contains(" THAT ") { score += 120 }
   if upper_text.contains("TION") { score += 180 }
   if upper_text.contains("THER") { score += 160 }
   if upper_text.contains("ING") { score += 120 }
   score
}

fn _swap_key_positions(str key, int i, int j) str {
   mut chars = []
   mut k = 0
   while k < key.len {
      chars = chars.append(utf8_slice(key, k, k + 1, 1))
      k += 1
   }
   def tmp = chars[i]
   chars[i] = chars[j]
   chars[j] = tmp
   join(chars, "")
}

fn _seed_keys(str base_key) list {
   mut seeds = []
   seeds = seeds.append(base_key)
   def swap_pairs = [
      [0, 1], [0, 2], [1, 2], [2, 3],
      [3, 4], [4, 5], [5, 6], [6, 7],
      [0, 4], [1, 5], [2, 6], [3, 7]
   ]
   mut i = 0
   while i < swap_pairs.len {
      def pair = swap_pairs[i]
      seeds = seeds.append(_swap_key_positions(base_key, pair[0], pair[1]))
      i += 1
   }
   seeds
}

fn substitution_apply_key(str text, str key, bool encrypt) str {
   "Apply a substitution key to text.
   key: 26-character string mapping A-Z(index 0=A, 1=B, ...).
   encrypt: true to encrypt, false to decrypt.
   Returns the transformed text preserving case and non-alpha chars."
   crypto_require(text != nil, "cipher.substitution_apply_key", "text is nil")
   crypto_require_len(key, 26, "cipher.substitution_apply_key", "key")
   mut scan = 0
   while scan < key.len {
      def c = ord(utf8_slice(key, scan, scan + 1, 1))
      if !_sub_is_alpha_code(c) { crypto_fail("cipher.substitution_apply_key", "key must contain only alphabetic characters") }
      scan += 1
   }
   def inv = encrypt ? nil : _sub_inverse_key_map(key)
   mut out = ""
   mut i = 0
   while i < text.len {
      def ch = utf8_slice(text, i, i + 1, 1)
      def code = ord(ch)
      case code {
         65..90 -> {
            def idx = code - 65
            if encrypt { out = str_add(out, upper(utf8_slice(key, idx, idx + 1, 1))) } else {
               def pos = inv[idx]
               if pos >= 0 { out = str_add(out, chr(65 + pos))
               } else { out = str_add(out, ch) }
            }
         }
         97..122 -> {
            def idx = code - 97
            if encrypt { out = str_add(out, lower(utf8_slice(key, idx, idx + 1, 1))) } else {
               def pos = inv[idx]
               if pos >= 0 { out = str_add(out, chr(97 + pos))
               } else { out = str_add(out, ch) }
            }
         }
         _ -> { out = str_add(out, ch) }
      }
      i += 1
   }
   out
}

fn substitution_encrypt(str text, str key) str {
   "Encrypt text using a monoalphabetic substitution key(26 chars for A-Z)."
   substitution_apply_key(text, key, true)
}

fn substitution_decrypt(str text, str key) str {
   "Decrypt text using a monoalphabetic substitution key(26 chars for A-Z)."
   substitution_apply_key(text, key, false)
}

fn substitution_tr(str text, str source, str target) str {
   "Translate each character from `source` to the character at the same index in `target`."
   crypto_require(text != nil, "cipher.substitution_tr", "text is nil")
   crypto_require(source != nil && target != nil, "cipher.substitution_tr", "source/target is nil")
   crypto_require(source.len == target.len, "cipher.substitution_tr", "source and target lengths differ")
   mut out = ""
   mut i = 0
   while i < text.len {
      def ch = utf8_slice(text, i, i + 1, 1)
      def pos = find(source, ch)
      out = str_add(out, pos >= 0 ? utf8_slice(target, pos, pos + 1, 1) : ch)
      i += 1
   }
   out
}

fn substitution_key_from_pairs(str plaintext, str ciphertext) str {
   "Derive a monoalphabetic substitution key from aligned known plaintext/ciphertext.
   Returns a 26-character encrypt key. Unknown plaintext letters are filled with
   unused cipher letters in alphabet order."
   crypto_require(plaintext != nil, "cipher.substitution_key_from_pairs", "plaintext is nil")
   crypto_require(ciphertext != nil, "cipher.substitution_key_from_pairs", "ciphertext is nil")
   crypto_require(plaintext.len == ciphertext.len, "cipher.substitution_key_from_pairs", "texts must be aligned and equal length")
   mut key = []
   mut i = 0
   while i < 26 {
      key = key.append("")
      i += 1
   }
   mut used_cipher = ""
   i = 0
   while i < plaintext.len {
      def pc = ord(utf8_slice(upper(plaintext), i, i + 1, 1))
      def cc = ord(utf8_slice(upper(ciphertext), i, i + 1, 1))
      def pa = _sub_is_alpha_code(pc)
      def ca = _sub_is_alpha_code(cc)
      if pa || ca {
         crypto_require(pa && ca, "cipher.substitution_key_from_pairs", "alpha/non-alpha mismatch")
         def pidx = pc - 65
         def cch = chr(cc)
         def prior = key[pidx]
         if prior != "" {
            crypto_require(prior == cch, "cipher.substitution_key_from_pairs", "conflicting plaintext mapping")
         } else {
            crypto_require(find(used_cipher, cch) < 0, "cipher.substitution_key_from_pairs", "conflicting ciphertext mapping")
            key[pidx] = cch
            used_cipher = str_add(used_cipher, cch)
         }
      }
      i += 1
   }
   mut fill = 0
   i = 0
   while i < 26 {
      if key[i] == "" {
         while fill < 26 && find(used_cipher, chr(65 + fill)) >= 0 { fill += 1 }
         crypto_require(fill < 26, "cipher.substitution_key_from_pairs", "no unused cipher letters left")
         def ch = chr(65 + fill)
         key[i] = ch
         used_cipher = str_add(used_cipher, ch)
      }
      i += 1
   }
   join(key, "")
}

fn substitution_freq_analysis(str text) list {
   "Compute letter frequency counts from text.
   Returns list of 26 counts(index 0=A, 25=Z)."
   crypto_require(text != nil, "cipher.substitution_freq_analysis", "text is nil")
   mut counts = []
   mut i = 0
   while i < 26 {
      counts = counts.append(0)
      i += 1
   }
   mut j = 0
   while j < text.len {
      def code = ord(utf8_slice(text, j, j + 1, 1))
      if code >= 65 && code <= 90 { counts[code - 65] = counts[code - 65] + 1 } elif code >= 97 && code <= 122 { counts[code - 97] = counts[code - 97] + 1 }
      j += 1
   }
   counts
}

fn substitution_freq_key(str ciphertext) str {
   "Build a frequency-seeded monoalphabetic substitution key guess.
   Returns a 26-char key string."
   crypto_require_nonempty(ciphertext, "cipher.substitution_crack_freq", "ciphertext")
   def counts = substitution_freq_analysis(ciphertext)
   mut indexed = []
   mut i = 0
   while i < 26 {
      indexed = indexed.append([counts[i], i])
      i += 1
   }
   mut n, j = indexed.len, 0
   while j < n - 1 {
      mut k = 0
      while k < n - 1 - j {
         if indexed[k][0] < indexed[k + 1][0] {
            def tmp = indexed[k]
            indexed[k] = indexed[k + 1]
            indexed[k + 1] = tmp
         }
         k += 1
      }
      j += 1
   }
   mut key = []
   mut ki = 0
   while ki < 26 {
      key = key.append(chr(65 + ki))
      ki += 1
   }
   mut pi = 0
   while pi < 26 && pi < ENGLISH_FREQ_ORDER.len {
      def ct_letter, pt_letter = indexed[pi][1], utf8_slice(ENGLISH_FREQ_ORDER, pi, pi + 1, 1)
      key[ct_letter] = pt_letter
      pi += 1
   }
   join(key, "")
}

fn substitution_crack_freq(str ciphertext) str {
   "Crack a monoalphabetic substitution cipher using frequency analysis.
   Maps the most frequent ciphertext letters to English frequency order.
   Returns a best-guess plaintext string."
   crypto_require_nonempty(ciphertext, "cipher.substitution_crack_freq", "ciphertext")
   def key_str = substitution_freq_key(ciphertext)
   substitution_decrypt(ciphertext, key_str)
}

fn substitution_crack_hill(str ciphertext, int rounds=4) list {
   "Refine a substitution crack using greedy pair-swaps over a frequency-seeded key.
   Returns [key, plaintext, score]."
   crypto_require_nonempty(ciphertext, "cipher.substitution_crack_hill", "ciphertext")
   mut best_key = substitution_freq_key(ciphertext)
   mut best_plain = substitution_decrypt(ciphertext, best_key)
   mut best_score = _sub_text_score(best_plain)
   mut round = 0
   while round < rounds {
      mut improved = false
      mut i = 0
      while i < 26 {
         mut j = i + 1
         while j < 26 {
            def cand_key = _swap_key_positions(best_key, i, j)
            def cand_plain = substitution_decrypt(ciphertext, cand_key)
            def cand_score = _sub_text_score(cand_plain)
            if cand_score > best_score {
               best_key = cand_key
               best_plain = cand_plain
               best_score = cand_score
               improved = true
            }
            j += 1
         }
         i += 1
      }
      if !improved { round = rounds } else { round += 1 }
   }
   [best_key, best_plain, best_score]
}

fn substitution_crack_hill_multistart(str ciphertext, int rounds=3) list {
   "Run several seeded hill-climbs and keep the best result.
   Returns [key, plaintext, score]."
   crypto_require_nonempty(ciphertext, "cipher.substitution_crack_hill_multistart", "ciphertext")
   def base_key = substitution_freq_key(ciphertext)
   def seeds = _seed_keys(base_key)
   mut best_key = base_key
   mut best_plain = substitution_decrypt(ciphertext, best_key)
   mut best_score = _sub_text_score(best_plain)
   mut i = 0
   while i < seeds.len {
      def seed = seeds[i]
      mut cur_key = seed
      mut cur_plain = substitution_decrypt(ciphertext, cur_key)
      mut cur_score = _sub_text_score(cur_plain)
      mut round = 0
      while round < rounds {
         mut improved = false
         mut a = 0
         while a < 26 {
            mut b = a + 1
            while b < 26 {
               def cand_key = _swap_key_positions(cur_key, a, b)
               def cand_plain = substitution_decrypt(ciphertext, cand_key)
               def cand_score = _sub_text_score(cand_plain)
               if cand_score > cur_score {
                  cur_key = cand_key
                  cur_plain = cand_plain
                  cur_score = cand_score
                  improved = true
               }
               b += 1
            }
            a += 1
         }
         if !improved { round = rounds } else { round += 1 }
      }
      if cur_score > best_score {
         best_key = cur_key
         best_plain = cur_plain
         best_score = cur_score
      }
      i += 1
   }
   [best_key, best_plain, best_score]
}

fn substitution_score(str text) int {
   "Score text by English-like letter frequency(higher = more English-like).
   Computes sum of abs(freq - expected) penalties."
   crypto_require(text != nil, "cipher.substitution_score", "text is nil")
   def counts = substitution_freq_analysis(text)
   mut total = 0
   mut i = 0
   while i < 26 {
      total = total + counts[i]
      i += 1
   }
   if total == 0 { return 0 }
   def expected = [8, 1, 3, 4, 13, 2, 2, 6, 7, 0, 1, 4, 2, 7, 8, 2, 0, 6, 6, 9, 3, 1, 2, 0, 2, 0]
   mut score = 0
   mut j = 0
   while j < 26 {
      def freq = counts[j] * 100 / total
      def diff = freq - expected[j]
      def absdiff = diff < 0 ? 0 - diff : diff
      score = score - absdiff
      j += 1
   }
   score
}

#main {
   def key = substitution_key_from_pairs("THEQUICKBROWN", "XQATZCDOHYFVE")
   assert(key == "BHDGAIJQCKOLMEFNTYPXZRVSUW", "known plaintext fills partial substitution key")
   assert(substitution_decrypt("XQA TZCDO HYFVE", key) == "THE QUICK BROWN", "derived key decrypts known pair")
   print("SUBSTITUTION_KNOWN_PLAINTEXT_OK")
   print("✓ std.math.crypto.cipher.substitution self-test passed")
}
