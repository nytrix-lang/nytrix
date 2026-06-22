;; Keywords: cipher playfair math crypto
;; Classical cipher routines for Playfair square construction, encryption, and decryption.
;; Reference:
;; - https://netlab.cs.ucla.edu/wiki/files/shannon1949.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap1.pdf
;; References:
;; - std.math.crypto.cipher
;; - std.math.crypto.analysis
module std.math.crypto.cipher.playfair(playfair_make_square, playfair_encrypt, playfair_decrypt, playfair_decrypt_offset, playfair_decrypt_both_offsets)
use std.core
use std.math.nt
use std.core.str

fn _playfair_builder_take(list b) str {
   def out = builder_to_str(b)
   builder_free(b)
   out
}

fn _playfair_pos_map(list square) dict {
   mut pos = dict(32)
   mut idx = 0
   while idx < 25 {
      pos = pos.set(to_str(load8(square[idx], 0)), idx)
      idx += 1
   }
   pos
}

fn _playfair_pair(list square, int idx1, int idx2) list {
   [load8(square[idx1], 0), load8(square[idx2], 0)]
}

fn playfair_make_square(str key) list {
   "Build a 5x5 Playfair key square from the given keyword.
   I and J are treated as the same letter(J is mapped to I).
   key: keyword string(case-insensitive, non-alpha ignored)
   Returns a flat list of 25 characters representing the 5x5 square row by row."
   mut square = list(0)
   mut used = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
   mut i = 0
   while i < key.len {
      def c = load8(key, i)
      mut letter = case c {
         65..90 -> c - 65
         97..122 -> c - 97
         _ -> -1
      }
      if letter >= 0 {
         letter = letter == 9 ? 8 : letter
         case used[letter] {
            0 -> {
               used[letter] = 1
               square = square.append(chr(letter + 65))
            }
            _ -> {}
         }
      }
      i += 1
   }
   mut j = 0
   while j < 26 {
      case j {
         9 -> {}
         _ -> {
            case used[j] {
               0 -> { square = square.append(chr(j + 65)) }
               _ -> {}
            }
         }
      }
      j += 1
   }
   square
}

fn playfair_find_pos(list square, any letter) list {
   "Find the row and column of a letter in the 5x5 Playfair square.
   square: flat list of 25 characters
   letter: uppercase character code to find
   Returns [row, col] as a list."
   def idx = _playfair_pos_map(square).get(to_str(letter), -1)
   if idx >= 0 {
      return [idx / 5, idx % 5]
   }
   [-1, -1]
}

fn playfair_prepare_digraphs(str plaintext) list {
   "Prepare plaintext for Playfair encryption by splitting into digraphs.
   Inserts 'X' between duplicate letters and pads with 'X' if odd length.
   plaintext: uppercase alphabetic string
   Returns a list of [char1, char2] digraph pairs."
   mut cleaned_b = Builder(plaintext.len + 8)
   mut i = 0
   while i < plaintext.len {
      def c = load8(plaintext, i)
      case c {
         65..90 -> { cleaned_b = builder_append(cleaned_b, chr(c)) }
         97..122 -> { cleaned_b = builder_append(cleaned_b, chr(c == 106 ? 73 : c - 32)) }
         _ -> {}
      }
      i += 1
   }
   def cleaned = _playfair_builder_take(cleaned_b)
   def n = cleaned.len
   case n {
      0 -> { return list(0) }
      _ -> {}
   }
   mut digraphs = list(0)
   mut pos = 0
   while pos < n {
      def c1 = load8(cleaned, pos)
      mut c2 = 88
      def next_pos = pos + 1
      if next_pos < n {
         def next_c = load8(cleaned, next_pos)
         if c1 == next_c { c2 = 88 } else {
            c2 = next_c
            pos += 1
         }
      }
      mut pair = list(0)
      pair = pair.append(c1)
      pair = pair.append(c2)
      digraphs = digraphs.append(pair)
      pos += 1
   }
   digraphs
}

fn _playfair_shift_digraph(list square, dict pos, int c1, int c2, int shift) list {
   "Transform a Playfair digraph by shifting same-row/same-column pairs."
   def idxp1, idxp2 = pos.get(to_str(c1), -1), pos.get(to_str(c2), -1)
   def r1 = idxp1 / 5
   def col1 = idxp1 % 5
   def r2 = idxp2 / 5
   def col2 = idxp2 % 5
   if r1 == r2 {
      return _playfair_pair(square, r1 * 5 + ((col1 + shift) % 5), r2 * 5 + ((col2 + shift) % 5))
   }
   if col1 == col2 {
      return _playfair_pair(square, ((r1 + shift) % 5) * 5 + col1, ((r2 + shift) % 5) * 5 + col2)
   }
   _playfair_pair(square, r1 * 5 + col2, r2 * 5 + col1)
}

fn _playfair_encrypt_digraph(list square, dict pos, int c1, int c2) list {
   "Encrypt a single Playfair digraph using the given square."
   _playfair_shift_digraph(square, pos, c1, c2, 1)
}

fn playfair_encrypt_digraph(list square, int c1, int c2) list {
   "Encrypt one Playfair digraph using a 5x5 key square."
   _playfair_encrypt_digraph(square, _playfair_pos_map(square), c1, c2)
}

fn _playfair_decrypt_digraph(list square, dict pos, int c1, int c2) list {
   "Decrypt a single Playfair digraph using the given square."
   _playfair_shift_digraph(square, pos, c1, c2, 4)
}

fn playfair_decrypt_digraph(list square, int c1, int c2) list {
   "Decrypt one Playfair digraph using a 5x5 key square."
   _playfair_decrypt_digraph(square, _playfair_pos_map(square), c1, c2)
}

fn playfair_encrypt(str plaintext, list square) str {
   "Encrypt plaintext using the Playfair cipher with the given 5x5 key square.
   plaintext: string to encrypt(non-alpha characters are stripped)
   square: flat list of 25 characters from playfair_make_square
   Returns the ciphertext as an uppercase string."
   def digraphs = playfair_prepare_digraphs(plaintext)
   def pos = _playfair_pos_map(square)
   mut result = Builder(digraphs.len * 2 + 8)
   mut i = 0
   while i < digraphs.len {
      def pair = digraphs[i]
      def c1 = pair[0]
      def c2 = pair[1]
      def enc = _playfair_encrypt_digraph(square, pos, c1, c2)
      def ch1 = enc[0]
      def ch2 = enc[1]
      result = builder_append(result, chr(ch1))
      result = builder_append(result, chr(ch2))
      i += 1
   }
   _playfair_builder_take(result)
}

fn playfair_decrypt(str ciphertext, list square) str {
   "Decrypt ciphertext using the Playfair cipher with the given 5x5 key square.
   ciphertext: uppercase alphabetic string to decrypt
   square: flat list of 25 characters from playfair_make_square
   Returns the decrypted uppercase string."
   def pos = _playfair_pos_map(square)
   mut result = Builder(ciphertext.len + 8)
   mut i = 0
   while i < ciphertext.len {
      def c1, c2 = load8(ciphertext, i), load8(ciphertext, i + 1)
      def dec = _playfair_decrypt_digraph(square, pos, c1, c2)
      def ch1 = dec[0]
      def ch2 = dec[1]
      result = builder_append(result, chr(ch1))
      result = builder_append(result, chr(ch2))
      i = i + 2
   }
   _playfair_builder_take(result)
}

fn playfair_decrypt_offset(str ciphertext, list square, int offset=0, bool strip_x=false) str {
   "Decrypt ciphertext after skipping an initial offset.
   Useful for captured Playfair streams that start mid-digraph. If the
   remaining ciphertext length is odd, the trailing byte is dropped."
   if offset < 0 { offset = 0 }
   if offset >= ciphertext.len { return "" }
   mut text = ciphertext
   if offset > 0 { text = slice(ciphertext, offset, ciphertext.len) }
   if (text.len % 2) == 1 { text = slice(text, 0, text.len - 1) }
   mut out = playfair_decrypt(text, square)
   if strip_x { out = str_replace(out, "X", "") }
   out
}

fn playfair_decrypt_both_offsets(str ciphertext, list square, bool strip_x=false) list {
   "Return the offset-0 and offset-1 Playfair decryptions for a ciphertext."
   [
      playfair_decrypt_offset(ciphertext, square, 0, strip_x),
      playfair_decrypt_offset(ciphertext, square, 1, strip_x)
   ]
}

#main {
   def square = playfair_make_square("PLAYFAIR EXAMPLE")
   assert(square.len == 25, "playfair square length")
   assert(square[0] == "P", "playfair square starts with key")
   assert(square[1] == "L", "playfair square preserves key order")
   def plaintext = "HIDETHEGOLDINTHETREXESTUMP"
   def ciphertext = playfair_encrypt(plaintext, square)
   assert(ciphertext == "BMODZBXDNABEKUDMUIXMMOUVIF", "classic playfair example encrypt")
   def decrypted = playfair_decrypt(ciphertext, square)
   assert(decrypted == plaintext, "classic playfair example decrypt")
   def noisy_square = playfair_make_square("play fair, jelly")
   def noisy_ct = playfair_encrypt("jolly balloon", noisy_square)
   def noisy_pt = playfair_decrypt(noisy_ct, noisy_square)
   assert(noisy_pt == "IOLXLYBALXLOON", "lowercase and J normalize through round trip")
   def both = playfair_decrypt_both_offsets("Z" + ciphertext, square, false)
   assert(both.len == 2, "both offsets returns two strings")
   assert(both[1] == plaintext, "offset-one decrypt recovers classic plaintext")
   def ct = playfair_encrypt("Hide the gold in the tree stump", square)
   assert(ct == "BMODZBXDNABEKUDMUIXMMOUVIF", "playfair known ciphertext")
   def pt = playfair_decrypt(ct, square)
   assert(pt == "HIDETHEGOLDINTHETREXESTUMP", "playfair decrypts known ciphertext")
   def offsets = playfair_decrypt_both_offsets("X" + ct, square, false)
   assert(offsets[1] == pt, "offset decrypt recovers shifted ciphertext")
   print("✓ std.math.crypto.cipher.playfair self-test passed")
}
