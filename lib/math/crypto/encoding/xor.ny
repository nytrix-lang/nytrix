;; Keywords: encoding xor math crypto
;; Encoding routines for XOR encoding, scoring, and key recovery.
;; Reference:
;; - https://netlab.cs.ucla.edu/wiki/files/shannon1949.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap1.pdf
;; References:
;; - std.math.crypto.encoding
;; - std.math.crypto
module std.math.crypto.encoding.xor(xor_with_single_byte, xor_with_repeating_key, xor_bytes_hex, single_byte_xor_bruteforce, english_score, repeating_key_xor_keylength, repeating_key_xor_crack, multi_text_xor_keystream, hamming_distance, crib_drag, xor_two_ciphertexts, repeating_xor_key_from_prefix)
use std.core
use std.math.bin
use std.core.str

fn _xor_overlap(list a, list b) list {
   def n = a.len < b.len ? a.len : b.len
   mut out = list(n)
   mut i = 0
   while i < n {
      out[i] = a[i] ^^ b[i]
      i += 1
   }
   store64(out, n, 0)
   out
}

fn xor_with_single_byte(list data, int key_byte) list {
   "XOR every byte in data with a single-byte key. Returns byte list."
   def n = data.len
   mut out = list(n)
   mut i = 0
   while i < n {
      out[i] = data[i] ^^ key_byte
      i += 1
   }
   store64(out, n, 0)
   out
}

fn xor_with_repeating_key(list data, list key) list {
   "XOR byte list data with a repeating byte-list key. Returns byte list."
   def key_len = key.len
   case key_len {
      0 -> { return [] }
      _ -> {}
   }
   def n = data.len
   mut out = list(n)
   mut i = 0
   while i < n {
      out[i] = data[i] ^^ key[i % key_len]
      i += 1
   }
   store64(out, n, 0)
   out
}

fn repeating_xor_key_from_prefix(list ciphertext, list known_prefix) list {
   "Recover a repeating-XOR key when the known prefix length is the key length."
   def n = known_prefix.len
   mut key = list(n)
   mut i = 0
   while i < n {
      key[i] = ciphertext[i] ^^ known_prefix[i]
      i += 1
   }
   store64(key, n, 0)
   key
}

fn xor_bytes_hex(str a_hex, str b_hex) str {
   "XOR two equal-length hex strings. Returns hex string."
   def a, b = a_hex.unhex, b_hex.unhex
   _xor_overlap(a, b).hex
}

fn hamming_distance(list a, list b) int {
   "Compute bit-level Hamming distance between two byte lists."
   def n = (a.len < b.len) ? a.len : b.len
   mut dist = 0
   mut i = 0
   while i < n {
      dist += bit_count(a[i] ^^ b[i])
      i += 1
   }
   dist
}

fn _english_byte_score(int b) int {
   mut score = 0
   case b {
      32..126 -> { score += 2 }
      9, 10, 13 -> { score += 1 }
      _ -> { score -= 5 }
   }
   case b {
      32, 69, 101 -> { score += 4 }
      65, 73, 78, 79, 83, 84, 97, 105, 110, 111, 115, 116, 95, 123, 125 -> { score += 3 }
      72, 104 -> { score += 2 }
      _ -> {}
   }
   score
}

fn english_score(list bytes) int {
   "Score a byte list by English letter frequency. Higher = more English-like.
   Considers etaoin shrdlu ordering, spaces, and printable chars."
   mut score = 0
   mut i = 0
   while i < bytes.len {
      score += _english_byte_score(int(bytes[i]))
      i += 1
   }
   score
}

fn _english_score_xor_key(list data, int key_byte) int {
   mut score = 0
   mut i = 0
   while i < data.len {
      score += _english_byte_score(int(data[i]) ^^ key_byte)
      i += 1
   }
   score
}

fn _english_score_xor_key_strided(list data, int start, int stride, int key_byte) int {
   mut score = 0
   mut i = start
   while i < data.len {
      score += _english_byte_score(int(data[i]) ^^ key_byte)
      i += stride
   }
   score
}

fn _single_byte_xor_key(list data) int {
   mut best_key = 0
   mut best_score = -1000000
   mut k = 0
   while k < 256 {
      def score = _english_score_xor_key(data, k)
      if score > best_score {
         best_score = score
         best_key = k
      }
      k += 1
   }
   best_key
}

fn single_byte_xor_bruteforce(list data) list {
   "Brute-force single-byte XOR key 0-255.
   Returns [best_key, plaintext_string, score]."
   mut best_key = 0
   mut best_score = -1000000
   mut k = 0
   while k < 256 {
      def score = _english_score_xor_key(data, k)
      if score > best_score {
         best_score = score
         best_key = k
      }
      k += 1
   }
   def best_plain = xor_with_single_byte(data, best_key).text
   [best_key, best_plain, best_score]
}

fn _single_byte_xor_key_strided(list data, int start, int stride) list {
   mut best_key = 0
   mut best_score = -1000000
   mut k = 0
   while k < 256 {
      def score = _english_score_xor_key_strided(data, start, stride, k)
      if score > best_score {
         best_score = score
         best_key = k
      }
      k += 1
   }
   [best_key, best_score]
}

fn repeating_key_xor_keylength(list ct, int min_len=2, int max_len=40) int {
   "Estimate repeating-key XOR key length using normalized Hamming distance across 4 block pairs.
   Returns best key length as int."
   def n_blocks = 4
   def ct_len = ct.len
   mut best_len = min_len
   mut best_dist = 1000000
   mut ks = min_len
   while ks <= max_len {
      if ks * n_blocks > ct_len { break }
      mut total_dist = 0
      mut pairs = 0
      mut bi = 0
      while bi + 1 < n_blocks {
         mut j = 0
         while j < ks {
            total_dist += bit_count(ct[bi * ks + j] ^^ ct[(bi + 1) * ks + j])
            j += 1
         }
         pairs += 1
         bi += 1
      }
      def norm = (total_dist * 1000) / (pairs * ks)
      if norm < best_dist {
         best_dist = norm
         best_len = ks
      }
      ks += 1
   }
   best_len
}

fn repeating_key_xor_crack(list ct, int key_len=0) list {
   "Crack repeating-key XOR. Auto-detects key length if key_len=0.
   Returns [key_bytes, plaintext_string]."
   def kl = (key_len == 0) ? repeating_key_xor_keylength(ct) : key_len
   mut key = list(kl)
   mut i = 0
   while i < kl {
      def res = _single_byte_xor_key_strided(ct, i, kl)
      key[i] = res[0]
      i += 1
   }
   store64(key, kl, 0)
   def pt_bytes = xor_with_repeating_key(ct, key)
   [key, pt_bytes.text]
}

fn multi_text_xor_keystream(list ciphertexts) list {
   "Recover keystream bytes from multiple ciphertexts encrypted with the same key(multi-time pad).
   Uses per-position frequency analysis. Returns keystream as byte list."
   if ciphertexts.len == 0 { return [] }
   mut max_len = 0
   mut i = 0
   while i < ciphertexts.len {
      def l = ciphertexts[i].len
      if l > max_len { max_len = l }
      i += 1
   }
   mut keystream = list(max_len)
   mut pos = 0
   while pos < max_len {
      mut col_bytes = list(ciphertexts.len)
      mut ci = 0
      mut col_n = 0
      while ci < ciphertexts.len {
         def ct = ciphertexts[ci]
         if pos < ct.len {
            col_bytes[col_n] = ct[pos]
            col_n += 1
         }
         ci += 1
      }
      store64(col_bytes, col_n, 0)
      keystream[pos] = _single_byte_xor_key(col_bytes)
      pos += 1
   }
   store64(keystream, max_len, 0)
   keystream
}

fn xor_two_ciphertexts(list ct1, list ct2) list {
   "XOR two ciphertexts of possibly different lengths. Returns XOR of overlapping bytes."
   _xor_overlap(ct1, ct2)
}

fn crib_drag(list ciphertext, list crib) list {
   "Slide a known plaintext 'crib' over a ciphertext XOR'd from a multi-time pad.
   At each position, XOR crib bytes with ciphertext to get candidate keystream bytes.
   Returns list of [position, candidate_keystream_bytes] for all positions."
   def ct = ciphertext
   def c_len = ct.len
   def cr_len = crib.len
   def out_n = c_len >= cr_len ? (c_len - cr_len + 1) : 0
   mut results = list(out_n)
   mut pos = 0
   while pos + cr_len <= c_len {
      mut ks_bytes = list(cr_len)
      mut i = 0
      while i < cr_len {
         ks_bytes[i] = ct[pos + i] ^^ crib[i]
         i += 1
      }
      store64(ks_bytes, cr_len, 0)
      results[pos] = [pos, ks_bytes]
      pos += 1
   }
   store64(results, pos, 0)
   results
}
