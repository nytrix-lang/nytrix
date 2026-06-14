;; Keywords: block-cipher stream stream-core math crypto
;; CTR and many-time-pad stream-cipher analysis routines.
;; References:
;; - std.math.crypto.block.stream
;; - std.math.crypto
module std.math.crypto.block.stream.core(ctr_xor_plaintexts, ctr_recover_keystream, ctr_bit_flip_byte, ctr_bit_flipping, ctr_score_english_byte, ctr_recover_periodic_keystream_english, ctr_apply_periodic_keystream, mtp_xor_all, mtp_guess_key_byte, mtp_crib_drag)
use std.core

fn _xor_overlap(list a, list b) list {
   def n = a.len < b.len ? a.len : b.len
   mut out = list(n)
   store64(out, n, 0)
   mut i = 0
   while i < n {
      __store_item_fast(out, i, __load_item_fast(a, i) ^^ __load_item_fast(b, i))
      i += 1
   }
   out
}

fn _ascii_base_score(int b) int {
   case int(b){
      9, 10, 13 -> 1
      32..126 -> 10
      _ -> -40
   }
}

fn _printable_score(int b) int {
   mut score = _ascii_base_score(b)
   score += b == 32 ? 9 : 0
   score += (b >= 65 && b <= 90) ? 4 : 0
   score += (b >= 97 && b <= 122) ? 5 : 0
   score += (b >= 48 && b <= 57) ? 1 : 0
   score += (b == 44 || b == 46 || b == 39 || b == 34 || b == 45 || b == 58 || b == 59 || b == 33 || b == 63) ? 2 : 0
   score += (b == 101 || b == 116 || b == 97 || b == 111 || b == 105 || b == 110 || b == 115 || b == 104 || b == 114 || b == 100 || b == 108 || b == 117) ? 7 : 0
   score += (b == 69 || b == 84 || b == 65 || b == 79 || b == 73 || b == 78 || b == 83 || b == 72 || b == 82 || b == 68 || b == 76 || b == 85) ? 5 : 0
   score
}

fn ctr_xor_plaintexts(list ct1, list ct2) list {
   "XOR two CTR ciphertexts encrypted with the same keystream, yielding p1 XOR p2 over the overlap."
   _xor_overlap(ct1, ct2)
}

fn ctr_recover_keystream(list ciphertext, list known_plaintext) list {
   "Recover keystream bytes from ciphertext and matching known plaintext bytes."
   _xor_overlap(ciphertext, known_plaintext)
}

fn ctr_bit_flip_byte(list ciphertext, int pos, int old_byte, int new_byte) list {
   "Flip one plaintext byte under CTR by editing ciphertext at the same position."
   if pos < 0 || pos >= ciphertext.len { return clone(ciphertext) }
   mut out = clone(ciphertext)
   out[pos] = out[pos] ^^ old_byte ^^ new_byte
   out
}

fn ctr_bit_flipping(list ciphertext, list edits) list {
   "Apply [pos, old_byte, new_byte] CTR bit-flip edits."
   mut out = clone(ciphertext)
   mut i = 0
   while i < edits.len {
      def e = edits.get(i, [])
      if is_list(e) && e.len >= 3 {
         def pos = int(e.get(0, -1))
         if pos >= 0 && pos < out.len { out[pos] = out[pos] ^^ int(e.get(1, 0)) ^^ int(e.get(2, 0)) }
      }
      i += 1
   }
   out
}

fn ctr_score_english_byte(int b) int {
   "Score one candidate plaintext byte for simple ASCII/English keystream recovery."
   _printable_score(b)
}

fn ctr_recover_periodic_keystream_english(list ciphertexts, int period=16) list {
   "Recover a repeated keystream period by independently scoring each byte position."
   if period <= 0 { return [] }
   mut keystream = list(period)
   mut pos = 0
   while pos < period {
      mut best_key = 0
      mut best_score = -1000000000
      mut guess = 0
      while guess < 256 {
         mut score = 0
         mut ci = 0
         while ci < ciphertexts.len {
            def ct = ciphertexts[ci]
            mut j = pos
            while j < ct.len {
               score += _printable_score(__load_item_fast(ct, j) ^^ guess)
               j += period
            }
            ci += 1
         }
         if score > best_score {
            best_score = score
            best_key = guess
         }
         guess += 1
      }
      __store_item_fast(keystream, pos, best_key)
      pos += 1
   }
   store64(keystream, period, 0)
   keystream
}

fn ctr_apply_periodic_keystream(list ciphertext, list keystream) list {
   "XOR ciphertext with a repeating keystream byte list."
   if keystream.len == 0 { return [] }
   def n = ciphertext.len
   def kn = keystream.len
   mut out = list(n)
   store64(out, n, 0)
   mut i = 0
   while i < n {
      __store_item_fast(out, i, __load_item_fast(ciphertext, i) ^^ __load_item_fast(keystream, i % kn))
      i += 1
   }
   out
}

fn mtp_xor_all(list ciphertexts) dict {
   "XOR all ciphertext pairs from a reused one-time-pad/multi-time-pad set."
   def n = ciphertexts.len
   mut result = dict((n * (n - 1)) / 2)
   mut i = 0
   while i < n {
      mut j = i + 1
      while j < n {
         result.set(i * n + j, _xor_overlap(ciphertexts.get(i), ciphertexts.get(j)))
         j += 1
      }
      i += 1
   }
   result
}

fn mtp_guess_key_byte(list ciphertexts, int position, int guess) dict {
   "Score a key-byte guess at one position across many ciphertexts."
   mut score = 0
   def n = ciphertexts.len
   mut plaintexts = list(n)
   mut valid = list(n)
   store64(plaintexts, n, 0)
   store64(valid, n, false)
   mut i = 0
   while i < n {
      def ct = ciphertexts.get(i)
      if position >= ct.len {
         __store_item_fast(plaintexts, i, 0)
         __store_item_fast(valid, i, false)
      } else {
         def b = __load_item_fast(ct, position) ^^ guess
         def ok = b == 0 || b == 10 || b == 13 || (b >= 32 && b <= 126)
         if ok { score += 1 }
         __store_item_fast(plaintexts, i, b)
         __store_item_fast(valid, i, ok)
      }
      i += 1
   }
   {"score": score, "plaintexts": plaintexts, "valid": valid}
}

fn mtp_crib_drag(list ciphertexts, list crib) list {
   "Drag a known byte-list crib across all ciphertext pairs and return likely keystream fragments."
   if ciphertexts.len < 2 || crib.len == 0 { return [] }
   mut matches = []
   mut i = 0
   while i < ciphertexts.len {
      mut j = i + 1
      while j < ciphertexts.len {
         def a, b = ciphertexts.get(i), ciphertexts.get(j)
         def limit = (a.len < b.len ? a.len : b.len) - crib.len
         mut pos = 0
         while pos <= limit && pos < 500 {
            mut score = 0
            mut k = 0
            while k < crib.len {
               if _printable_score((a[pos + k] ^^ b[pos + k]) ^^ crib[k]) > 0 { score += 1 }
               k += 1
            }
            if score >= crib.len - 1 {
               mut ks = list(crib.len)
               k = 0
               while k < crib.len {
                  ks[k] = a[pos + k] ^^ crib[k]
                  k += 1
               }
               store64(ks, crib.len, 0)
               matches = matches.append({"ct_idx": i, "ct_idx2": j, "position": pos, "keystream": ks})
            }
            pos += 1
         }
         j += 1
      }
      i += 1
   }
   matches
}
