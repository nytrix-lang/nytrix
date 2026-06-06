;; Keywords: cipher analyze cryptanalysis scoring frequency math crypto analysis
;; Cipher analysis helpers for ranking classical cipher candidates without the legacy autocrack API name.
;; References:
;; - std.math.crypto.analysis.worldfreq
;; - std.math.crypto.cipher
module std.math.crypto.cipher.analyze(analyze, analyze_best)
use std.core
use std.math.bin (hex_is_valid, hex_normalize)
use std.math.crypto.error
use std.math.crypto.cipher.caesar
use std.math.crypto.cipher.affine
use std.math.crypto.cipher.rail_fence
use std.math.crypto.cipher.keyboard_shift
use std.math.crypto.cipher.substitution
use std.math.crypto.cipher.vigenere
use std.math.crypto.analysis.worldfreq (
   worldfreq_alpha_upper, worldfreq_word_stats, worldfreq_ngram_score,
)

use std.math.crypto.encoding.xor

@inline
fn _is_alpha_byte(int c) bool {
   case c {
      65..90, 97..122 -> true
      _ -> false
   }
}

@inline
fn _is_printable_byte(int c) bool {
   case c {
      9, 10, 13 -> true
      32..126 -> true
      _ -> false
   }
}

@inline
fn _is_upper_vowel_byte(int c) bool {
   case c {
      65, 69, 73, 79, 85, 89 -> true
      _ -> false
   }
}

@inline
fn _is_digit_byte(int c) bool {
   case c {
      48..57 -> true
      _ -> false
   }
}

@inline
fn _is_symbol_byte(int c) bool {
   case c {
      33..64, 91..96, 123..126 -> true
      _ -> false
   }
}

fn _excess_run(int run, int threshold) int {
   run >= threshold ? run - (threshold - 1) : 0
}

fn _shape_penalty(str text) int {
   def bs = text.to_bytes
   if(bs.len == 0){ return -6000 }
   mut vowels, alpha, bad_runs, current_run, same_run, prev, i = 0, 0, 0, 0, 0, -1, 0
   while(i < bs.len){
      def b = bs.get(i)
      if(_is_alpha_byte(b)){
         alpha += 1
         def u = (b >= 97) ? (b - 32) : b
         if(_is_upper_vowel_byte(u)){ vowels += 1 }
         current_run += 1
      } else {
         bad_runs += _excess_run(current_run, 8)
         current_run = 0
      }
      if(b == prev){ same_run += 1 } else {
         bad_runs += _excess_run(same_run, 5)
         same_run = 1
      }
      prev = b
      i += 1
   }
   bad_runs += _excess_run(current_run, 8)
   bad_runs += _excess_run(same_run, 5)
   mut penalty = bad_runs * 180
   if(alpha > 0){
      def vowel_pct = vowels * 100 / alpha
      if(vowel_pct < 20){ penalty += (20 - vowel_pct) * 12 }
      if(vowel_pct > 58){ penalty += (vowel_pct - 58) * 8 }
   }
   -penalty
}

fn _printable_bonus(str text) int {
   def bs = text.to_bytes
   if(bs.len == 0){ return -5000 }
   mut printable, alpha, weird, i = 0, 0, 0, 0
   while(i < bs.len){
      def b = bs.get(i)
      if(_is_printable_byte(b)){ printable += 1 } else { weird += 1 }
      if(_is_alpha_byte(b)){ alpha += 1 }
      i += 1
   }
   (printable * 800 / bs.len) + (alpha * 200 / bs.len) - weird * 80
}

fn _word_boundary_bonus(str text) int {
   def n = text.len
   if(n == 0){ return -5000 }
   def counts = _text_counts(text)
   def spaces = counts.get(2, 0)
   def alpha = counts.get(0, 0)
   if(alpha == 0){ return -4000 }
   mut bonus = 0
   def space_ratio = spaces * 100 / n
   if(space_ratio >= 8 && space_ratio <= 24){ bonus += 300 }
   if(space_ratio == 0 && n > 12){ bonus -= 250 }
   if(space_ratio > 35){ bonus -= 250 }
   bonus
}

fn _deep_text_score(str text) int {
   def upper_text = upper(text)
   def alpha_text = worldfreq_alpha_upper(upper_text)
   def word_stats = worldfreq_word_stats(upper_text)
   def hexish = hex_is_valid(text)
   mut score = _quick_text_score(text) + int(word_stats.get(0, 0)) + worldfreq_ngram_score(alpha_text)
   if(hexish && text.len >= 16){ score -= 2600 } elif(hexish && int(word_stats.get(2, 0)) == 0){ score -= 2200 }
   score
}

fn _quick_text_score(str text) int {
   english_score(text.to_bytes) +
   _printable_bonus(text) +
   _shape_penalty(text) +
   _word_boundary_bonus(text)
}

fn _text_counts(str text) list {
   mut alpha, digits, spaces, symbols, i = 0, 0, 0, 0, 0
   while(i < text.len){
      def c = load8(text, i)
      if(_is_alpha_byte(c)){ alpha += 1 }
      if(_is_digit_byte(c)){ digits += 1 }
      if(c == 32){ spaces += 1 }
      if(_is_symbol_byte(c)){ symbols += 1 }
      i += 1
   }
   [alpha, digits, spaces, symbols]
}

fn _push_candidate(list cands, str kind, any param, str plaintext, int score) list {
   mut i = 0
   while(i < cands.len){
      def row = cands.get(i)
      if(row.get(2) == plaintext){
         if(score > row.get(3)){
            cands[i] = [kind, param, plaintext, score]
         }
         return cands
      }
      i += 1
   }
   cands.append([kind, param, plaintext, score])
}

fn _sort_candidates_desc(list cands) list {
   def out = clone(cands)
   mut i = 1
   while(i < out.len){
      def row = out.get(i)
      def row_score = row.get(3, 0)
      mut j = i - 1
      while(j >= 0 && out.get(j).get(3, 0) < row_score){
         out[j + 1] = out.get(j)
         j -= 1
      }
      out[j + 1] = row
      i += 1
   }
   out
}

fn _add_xor_hex_candidates(list candidates, str ciphertext) list {
   def raw = hex_normalize(ciphertext).unhex
   mut cands = candidates
   mut key = 0
   while(key < 256){
      def pt = xor_with_single_byte(raw, key).text
      cands = _push_candidate(cands, "single_byte_xor_hex", key, pt, _quick_text_score(pt))
      key += 1
   }
   cands
}

fn _add_caesar_candidates(list candidates, str ciphertext) list {
   def caesar = caesar_bruteforce(ciphertext)
   mut cands = candidates
   mut i = 0
   while(i < caesar.len){
      def row = caesar.get(i)
      def pt = row.get(1)
      cands = _push_candidate(cands, "caesar", row.get(0), pt, _quick_text_score(pt))
      i += 1
   }
   cands
}

fn _add_affine_candidates(list candidates, str ciphertext) list {
   def a_keys = [1, 3, 5, 7, 9, 11, 15, 17, 19, 21, 23, 25]
   mut cands = candidates
   mut ai = 0
   while(ai < a_keys.len){
      def a = a_keys.get(ai)
      mut b = 0
      while(b < 26){
         def pt = affine_decrypt(ciphertext, a, b, 26)
         if(pt != nil){ cands = _push_candidate(cands, "affine", [a, b], pt, _quick_text_score(pt)) }
         b += 1
      }
      ai += 1
   }
   cands
}

fn _add_keyboard_shift_candidates(list candidates, str ciphertext) list {
   mut cands = candidates
   mut shift = -6
   while(shift <= 6){
      if(shift != 0){
         def pt = keyboard_shift_transform(ciphertext, shift)
         cands = _push_candidate(cands, "keyboard_shift", shift, pt, _quick_text_score(pt))
      }
      shift += 1
   }
   cands
}

fn _add_rail_fence_candidates(list candidates, str ciphertext, int limit) list {
   mut max_rails = ciphertext.len / 2
   if(max_rails > limit){ max_rails = limit }
   if(max_rails < 2){ max_rails = 2 }
   mut cands = candidates
   mut rails = 2
   while(rails <= max_rails){
      def pt = rail_fence_decrypt(ciphertext, rails)
      cands = _push_candidate(cands, "rail_fence", rails, pt, _quick_text_score(pt))
      rails += 1
   }
   cands
}

fn _add_vigenere_candidates(list candidates, str ciphertext) list {
   def results = vigenere_crack(ciphertext)
   mut cands = candidates
   mut i = 0
   while(i < results.len && i < 4){
      def best = results.get(i)
      def pt = best.get(2)
      cands = _push_candidate(cands, "vigenere", best.get(1), pt, _deep_text_score(pt))
      i += 1
   }
   cands
}

fn _add_substitution_candidate(list candidates, str ciphertext) list {
   def cracked = substitution_crack_hill_multistart(ciphertext, 4)
   def pt = cracked.get(1)
   _push_candidate(candidates, "substitution_hill", "freq+swap", pt, _deep_text_score(pt) + substitution_score(pt))
}

fn analyze(str ciphertext, int limit=12) list {
   "Try a set of classical-cipher cracking heuristics without knowing the key.
   Returns a list of candidates [kind, param, plaintext, score]."
   crypto_require_nonempty(ciphertext, "cipher.analyze", "ciphertext")
   mut candidates = []
   def n = ciphertext.len
   def counts = _text_counts(ciphertext)
   def alpha_n = counts.get(0, 0)
   def digit_n = counts.get(1, 0)
   def hexish_cipher = hex_is_valid(ciphertext)
   if(hexish_cipher){ return _sort_candidates_desc(_add_xor_hex_candidates(candidates, ciphertext)) }
   def space_n = counts.get(2, 0)
   def non_alpha_printable_n = counts.get(3, 0)
   def mostly_alpha = alpha_n * 100 >= n * 70
   def has_spaces = space_n > 0
   def compact_alpha = mostly_alpha && !has_spaces
   def has_separators = digit_n > 0 || ciphertext.contains("_") || ciphertext.contains("-") || ciphertext.contains(":") || ciphertext.contains("=")
   if(alpha_n > 0){ candidates = _add_caesar_candidates(candidates, ciphertext) }
   if(alpha_n > 0 && n <= 160 && ciphertext == upper(ciphertext)){
      candidates = _add_affine_candidates(candidates, ciphertext)
   }
   if((non_alpha_printable_n > 0 || digit_n > 0) && n <= 160){
      candidates = _add_keyboard_shift_candidates(candidates, ciphertext)
   }
   if((has_spaces || has_separators) && n >= 8){
      candidates = _add_rail_fence_candidates(candidates, ciphertext, limit)
   }
   if(compact_alpha && n >= 24){
      candidates = _add_vigenere_candidates(candidates, ciphertext)
      candidates = _add_substitution_candidate(candidates, ciphertext)
   }
   _sort_candidates_desc(candidates)
}

fn analyze_best(str ciphertext, int limit=12) list {
   "Return the single best auto-crack candidate as [kind, param, plaintext, score]."
   crypto_require_nonempty(ciphertext, "cipher.analyze_best", "ciphertext")
   def candidates = analyze(ciphertext, limit)
   if(candidates.len == 0){ return ["identity", "", ciphertext, _quick_text_score(ciphertext)] }
   candidates.get(0)
}
