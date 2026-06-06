;; Keywords: cipher vigenere math crypto
;; Vigenere cipher encryption, decryption, and key recovery routines.
;; Reference:
;; - https://netlab.cs.ucla.edu/wiki/files/shannon1949.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap1.pdf
;; References:
;; - std.math.crypto.cipher
;; - std.math.crypto.analysis
module std.math.crypto.cipher.vigenere(vigenere_encrypt, vigenere_decrypt, vigenere_crack, vigenere_guess_key_lengths, index_of_coincidence, kasiski_test)
use std.core
use std.core.str (Builder, builder_append, builder_append_byte, builder_to_str, builder_free)
use std.math.crypto.error

def _VG_ENG_FREQ = [82, 15, 28, 43, 127, 22, 20, 61, 70, 2, 8, 40, 24, 67, 75, 19, 1, 60, 63, 91, 28, 10, 24, 2, 20, 2]

fn _vigenere_require_key(str key, str scope) bool {
   crypto_require_nonempty(key, scope, "key")
   mut i = 0
   while(i < key.len){
      def c = load8(key, i)
      case c {
         65..90, 97..122 -> {}
         _ -> { crypto_fail(scope, "key must contain only alphabetic characters") }
      }
      i += 1
   }
   true
}

fn _vg_builder_take(list b) str {
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn _vg_zero_list(int n) list {
   mut xs = list(n)
   mut i = 0
   while(i < n){
      xs[i] = 0
      i += 1
   }
   store64(xs, n, 0)
   xs
}

fn _vg_english_byte_score(int b) int {
   mut score = 0
   case b {
      32..126 -> { score += 2 }
      9, 10, 13 -> { score += 1 }
      _ -> { score -= 5 }
   }
   case b {
      32, 69 -> { score += 4 }
      65, 73, 78, 79, 83, 84, 95, 123, 125 -> { score += 3 }
      72 -> { score += 2 }
      _ -> {}
   }
   score
}

fn _vigenere_plain_score(str text) int {
   mut score = 0
   mut has_ing = false
   mut has_tion = false
   mut i = 0
   while(i < text.len){
      def c = load8(text, i)
      score += _vg_english_byte_score(c)
      if(!has_ing && i + 2 < text.len && c == 73 && load8(text, i + 1) == 78 && load8(text, i + 2) == 71){ has_ing = true }
      if(!has_tion && i + 3 < text.len && c == 84 && load8(text, i + 1) == 73 && load8(text, i + 2) == 79 && load8(text, i + 3) == 78){ has_tion = true }
      i += 1
   }
   if(has_ing){ score += 80 }
   if(has_tion){ score += 120 }
   score
}

fn _vg_clean_alpha(str text) str {
   mut out = Builder(max(16, text.len + 8))
   mut i = 0
   while(i < text.len){
      def c = load8(text, i)
      case c {
         65..90 -> { out = builder_append_byte(out, c) }
         97..122 -> { out = builder_append_byte(out, c - 32) }
         _ -> {}
      }
      i += 1
   }
   _vg_builder_take(out)
}

fn _vg_avg_ic_for_len(str text, int key_len) f64 {
   if(key_len <= 0){ return 0.0 }
   def width = key_len * 26
   mut counts = _vg_zero_list(width)
   mut lens = _vg_zero_list(key_len)
   mut alpha_pos = 0
   mut i = 0
   while(i < text.len){
      def c = load8(text, i)
      case c {
         65..90 -> {
            def col = alpha_pos % key_len
            counts[col * 26 + c - 65] = counts[col * 26 + c - 65] + 1
            lens[col] = lens[col] + 1
            alpha_pos += 1
         }
         97..122 -> {
            def col = alpha_pos % key_len
            counts[col * 26 + c - 97] = counts[col * 26 + c - 97] + 1
            lens[col] = lens[col] + 1
            alpha_pos += 1
         }
         _ -> {}
      }
      i += 1
   }
   mut total = 0.0
   mut count = 0
   mut pos = 0
   while(pos < key_len){
      def n = lens[pos]
      if(n > 1){
         mut sum = 0
         mut j = 0
         def base = pos * 26
         while(j < 26){
            def f = counts[base + j]
            sum = sum + f * (f - 1)
            j += 1
         }
         total = total + float(sum) / float(n * (n - 1))
         count += 1
      }
      pos += 1
   }
   count == 0 ? 0.0 : total / count
}

fn _vg_key_char_from_counts(list counts, int base, int n) str {
   if(n <= 0){ return "A" }
   mut best_shift = 0
   mut best_chi = 999999999
   mut shift = 0
   while(shift < 26){
      mut chi = 0
      mut letter = 0
      while(letter < 26){
         def expected = _VG_ENG_FREQ[letter]
         def shifted = (letter + shift) % 26
         def count = counts[base + shifted]
         def obs = count * 1000 / n
         chi = chi + (obs - expected) * (obs - expected) / expected
         letter += 1
      }
      if(chi < best_chi){
         best_chi = chi
         best_shift = shift
      }
      shift += 1
   }
   chr(best_shift + 65)
}

fn _vg_key_for_len(str clean, int key_len) str {
   def width = key_len * 26
   mut counts = _vg_zero_list(width)
   mut lens = _vg_zero_list(key_len)
   mut i = 0
   while(i < clean.len){
      def c = load8(clean, i)
      case c {
         65..90 -> {
            def col = i % key_len
            counts[col * 26 + c - 65] = counts[col * 26 + c - 65] + 1
            lens[col] = lens[col] + 1
         }
         _ -> {}
      }
      i += 1
   }
   mut guessed_key = Builder(max(16, key_len + 8))
   mut pos = 0
   while(pos < key_len){
      guessed_key = builder_append(guessed_key, _vg_key_char_from_counts(counts, pos * 26, lens[pos]))
      pos += 1
   }
   _vg_builder_take(guessed_key)
}

fn _vg_sort_score_rows_desc(list rows) list {
   def out = clone(rows)
   mut i = 1
   while(i < out.len){
      def row = out.get(i)
      def score = row.get(0, 0)
      mut j = i - 1
      while(j >= 0 && out.get(j).get(0, 0) < score){
         out[j + 1] = out.get(j)
         j -= 1
      }
      out[j + 1] = row
      i += 1
   }
   out
}

fn _vg_kasiski_factor_counts(str text, int max_len) list {
   mut counts = _vg_zero_list(max_len + 1)
   def text_len = text.len
   if(text_len < 3){ return counts }
   mut last3 = _vg_zero_list(26 * 26 * 26)
   mut pos = 0
   while(pos + 2 < text_len){
      def a = load8(text, pos) - 65
      def b = load8(text, pos + 1) - 65
      def c = load8(text, pos + 2) - 65
      def p3 = (a * 26 + b) * 26 + c
      def prev3 = last3[p3]
      if(prev3 > 0){
         def dist = pos - (prev3 - 1)
         mut k = 2
         while(k <= max_len){
            if(dist % k == 0){ counts[k] = counts[k] + 1 }
            k += 1
         }
      }
      last3[p3] = pos + 1
      pos += 1
   }
   counts
}

fn _vg_apply_shift(int value, int key_val, bool decrypt) int {
   if(decrypt){ return(value - key_val + 26) % 26 }
   (value + key_val) % 26
}

fn _vg_transform(str text, str key, bool decrypt, str scope, str name) str {
   crypto_require(text != nil, scope, name + " is nil")
   _vigenere_require_key(key, scope)
   mut result = Builder(max(16, text.len + 8))
   mut ki = 0
   def key_len = key.len
   mut i = 0
   while(i < text.len){
      def c = load8(text, i)
      case c {
         65..90 -> {
            def key_val = load8(key, ki % key_len) - 65
            result = builder_append_byte(result, _vg_apply_shift(c - 65, key_val, decrypt) + 65)
            ki += 1
         }
         97..122 -> {
            def key_val = load8(key, ki % key_len) - 65
            def out_base = decrypt ? 97 : 65
            result = builder_append_byte(result, _vg_apply_shift(c - 97, key_val, decrypt) + out_base)
            ki += 1
         }
         _ -> { result = builder_append_byte(result, c) }
      }
      i += 1
   }
   _vg_builder_take(result)
}

fn _vg_guess_key_len_rows_clean(str clean, int max_len) list {
   max_len = max(1, max_len)
   mut scores = list(max_len)
   def kasiski_counts = _vg_kasiski_factor_counts(clean, max_len)
   mut key_len = 1
   while(key_len <= max_len){
      def avg_ic = _vg_avg_ic_for_len(clean, key_len)
      def ic_score = int(avg_ic * 100000)
      def kasiski_score = kasiski_counts[key_len] * 250
      def short_penalty = key_len > 1 ? 0 : 300
      scores[key_len - 1] = [ic_score + kasiski_score - short_penalty, key_len, int(avg_ic * 10000)]
      key_len += 1
   }
   _vg_sort_score_rows_desc(scores)
}

fn vigenere_guess_key_lengths(str ciphertext, int max_len=20) list {
   "Guess likely Vigenere key lengths using Kasiski distances and average IC.
   Returns a list of candidate key lengths ordered best-first."
   crypto_require_nonempty(ciphertext, "cipher.vigenere_guess_key_lengths", "ciphertext")
   def clean = _vg_clean_alpha(ciphertext)
   if(clean.len == 0){ return [] }
   def scores = _vg_guess_key_len_rows_clean(clean, max_len)
   mut out = list(scores.len)
   mut i = 0
   while(i < scores.len){
      out[i] = scores[i][1]
      i += 1
   }
   store64(out, scores.len, 0)
   out
}

fn vigenere_encrypt(str plaintext, str key) str {
   "Encrypt plaintext using Vigenere polyalphabetic substitution cipher with the given keyword.
   plaintext: uppercase alphabetic string to encrypt
   key: uppercase alphabetic keyword
   Returns ciphertext as an uppercase string of the same length."
   _vg_transform(plaintext, key, false, "cipher.vigenere_encrypt", "plaintext")
}

fn vigenere_decrypt(str ciphertext, str key) str {
   "Decrypt ciphertext that was encrypted with the Vigenere cipher using the given keyword.
   ciphertext: uppercase alphabetic string to decrypt
   key: uppercase alphabetic keyword used during encryption
   Returns the original plaintext as an uppercase string."
   _vg_transform(ciphertext, key, true, "cipher.vigenere_decrypt", "ciphertext")
}

fn index_of_coincidence(str text) f64 {
   "Compute the index of coincidence for the given text.
   text: alphabetic string(case-insensitive, non-alpha ignored)
   Returns the IC value as a float. Higher values indicate non-random text."
   crypto_require(text != nil, "cipher.index_of_coincidence", "text is nil")
   mut freq_table = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
   mut n = 0
   mut i = 0
   while(i < text.len){
      def c = load8(text, i)
      case c {
         65..90 -> {
            def idx = c - 65
            freq_table[idx] = freq_table[idx] + 1
            n += 1
         }
         97..122 -> {
            def idx = c - 97
            freq_table[idx] = freq_table[idx] + 1
            n += 1
         }
         _ -> {}
      }
      i += 1
   }
   if(n <= 1){ return 0.0 }
   mut sum = 0
   mut j = 0
   while(j < 26){
      def f = freq_table[j]
      sum = sum + f * (f - 1)
      j += 1
   }
   float(sum) / float(n * (n - 1))
}

fn kasiski_test(str text) list {
   "Find repeated sequences of length 3 or more in the text and compute the distances between their occurrences.
   text: alphabetic string to analyze
   Returns a list of distance values between repeated sequence occurrences."
   crypto_require_nonempty(text, "cipher.kasiski_test", "text")
   def text_len = text.len
   mut distances = list(0)
   mut seq_len = 3
   mut match_cap = 256
   mut search_window = 2048
   while(seq_len <= 5){
      if(seq_len > text_len){ seq_len += 1 }
      mut pos = 0
      while(pos <= text_len - seq_len){
         mut next_pos = pos + seq_len
         mut hits = 0
         while(next_pos <= text_len - seq_len && next_pos <= pos + search_window){
            mut is_match = true
            mut j = 0
            while(j < seq_len){
               def c1, c2 = load8(text, pos + j), load8(text, next_pos + j)
               if(c1 != c2){ is_match = false }
               j += 1
            }
            if(is_match){
               def dist = next_pos - pos
               distances = distances.append(dist)
               hits += 1
               if(hits >= match_cap){ next_pos = text_len }
            }
            next_pos += 1
         }
         pos += 1
      }
      seq_len += 1
   }
   distances
}

fn vigenere_crack(str ciphertext) list {
   "Attempt to crack Vigenere cipher by estimating key length via index of coincidence,
   then performing frequency analysis on each key position.
   ciphertext: uppercase alphabetic string encrypted with Vigenere
   Returns a list of [guessed_key_length, guessed_key, decrypted_text] entries."
   crypto_require_nonempty(ciphertext, "cipher.vigenere_crack", "ciphertext")
   def clean = _vg_clean_alpha(ciphertext)
   def text_len = clean.len
   mut ranked = list(8)
   mut ranked_len = 0
   mut lens = _vg_guess_key_len_rows_clean(clean, 20)
   mut li = 0
   while(li < lens.len && li < 8){
      def key_len = lens[li][1]
      if(key_len > 0 && key_len <= text_len){
         def guessed_key_s = _vg_key_for_len(clean, key_len)
         def decrypted = vigenere_decrypt(clean, guessed_key_s)
         mut score = _vigenere_plain_score(decrypted)
         def ic_scaled = lens[li][2]
         if(ic_scaled >= 550 && ic_scaled <= 750){ score += 350 }
         if(ic_scaled < 350){ score -= 500 }
         ranked[ranked_len] = [score, key_len, guessed_key_s, decrypted]
         ranked_len += 1
      }
      li += 1
   }
   store64(ranked, ranked_len, 0)
   ranked = _vg_sort_score_rows_desc(ranked)
   def result_n = min(5, ranked.len)
   mut result = list(result_n)
   mut i = 0
   while(i < result_n){
      def row = ranked[i]
      result[i] = [row[1], row[2], row[3]]
      i += 1
   }
   store64(result, result_n, 0)
   result
}

#main {
   def f64: ic = index_of_coincidence("ABAB")
   assert(ic > 0.33 && ic < 0.34, "index_of_coincidence uses floating division")
   print("✓ std.math.crypto.cipher.vigenere self-test passed")
}
