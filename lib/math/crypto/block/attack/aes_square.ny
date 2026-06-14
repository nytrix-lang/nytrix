;; Keywords: block-cipher attack aes-square math crypto
;; Block-cipher attack routines for AES Square attack.
;; Reusable APIs for state transforms, key schedule, delta-set generation,
;; and key recovery.
;; Reference:
;; - https://www.davidwong.fr/blockbreakers/
;; References:
;; - std.math.crypto.block.attack
;; - std.math.crypto
module std.math.crypto.block.attack.aes_square(aes_square_encrypt_block, aes_square_transform_state, aes_square_sub_bytes, aes_square_shift_rows, aes_square_mix_columns, aes_square_add_round_key, aes_square_key_expand, aes_square_reverse_key_expand, aes_square_recover_first_key, aes_square_state_from_rows, aes_square_state_to_rows, aes_square_get_delta_set, aes_square_encrypt_delta_set, aes_square_gather_encrypted_delta_sets, aes_square_reverse_state, aes_square_is_guess_correct, aes_square_guess_position, aes_square_crack_last_key, aes_square_crack_key)
use std.core
use std.math.bin
use std.math.crypto.error

def _SBOX = [0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16]
def _INV_SBOX = [0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb, 0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb, 0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e, 0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25, 0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92, 0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84, 0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06, 0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b, 0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73, 0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e, 0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b, 0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4, 0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f, 0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef, 0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61, 0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d]

fn _word_xor(list w1, list w2) list {
   [
      (w1.get(0, 0) ^^ w2.get(0, 0)) & 255,
      (w1.get(1, 0) ^^ w2.get(1, 0)) & 255,
      (w1.get(2, 0) ^^ w2.get(2, 0)) & 255,
      (w1.get(3, 0) ^^ w2.get(3, 0)) & 255
   ]
}

fn _word_rot(list word) list {
   error.crypto_require_len(word, 4, "block.aes_square", "word")
   [word.get(1, 0), word.get(2, 0), word.get(3, 0), word.get(0, 0)]
}

fn _word_sub(list word) list {
   error.crypto_require_len(word, 4, "block.aes_square", "word")
   [
      _SBOX.get(word.get(0, 0) & 255, 0),
      _SBOX.get(word.get(1, 0) & 255, 0),
      _SBOX.get(word.get(2, 0) & 255, 0),
      _SBOX.get(word.get(3, 0) & 255, 0)
   ]
}

fn _gf_mul2(int x) int {
   def b, y = x & 255, (b << 1) & 255
   ((b & 128) != 0) ? (y ^^ 0x1b) : y
}

fn _gf_mul3(int x) int { (_gf_mul2(x) ^^ (x & 255)) & 255 }

fn _rcon_word(int round_idx) list {
   error.crypto_require(round_idx >= 1 && round_idx < 256, "block.aes_square", "round_idx must be in [1,255]")
   mut v, i = 1, 1
   while i < round_idx {
      v = _gf_mul2(v)
      i += 1
   }
   [v & 255, 0, 0, 0]
}

fn _key_to_words(list key) list {
   error.crypto_require_len(key, 16, "block.aes_square", "key")
   [
      [key.get(0, 0), key.get(1, 0), key.get(2, 0), key.get(3, 0)],
      [key.get(4, 0), key.get(5, 0), key.get(6, 0), key.get(7, 0)],
      [key.get(8, 0), key.get(9, 0), key.get(10, 0), key.get(11, 0)],
      [key.get(12, 0), key.get(13, 0), key.get(14, 0), key.get(15, 0)]
   ]
}

fn _words_to_key(list words) list {
   error.crypto_require(words != nil && is_list(words), "block.aes_square", "words must be a list")
   mut out = []
   mut wi = 0
   while wi < words.len {
      def w = words.get(wi, [])
      error.crypto_require_len(w, 4, "block.aes_square", "word")
      out = out.append(w.get(0, 0) & 255)
      out = out.append(w.get(1, 0) & 255)
      out = out.append(w.get(2, 0) & 255)
      out = out.append(w.get(3, 0) & 255)
      wi += 1
   }
   out
}

fn aes_square_key_expand(list key, int rounds=11) list {
   "Expand an AES-128 key into round words.
   key: 16-byte list
   rounds: number of round keys to generate(AES-128 full = 11)
   Returns a list of 4-byte words, length = rounds * 4."
   error.crypto_require(rounds >= 1, "block.aes_square", "rounds must be >= 1")
   def init_words = _key_to_words(key)
   mut out = []
   out = out.append(init_words.get(0))
   out = out.append(init_words.get(1))
   out = out.append(init_words.get(2))
   out = out.append(init_words.get(3))
   mut prev0, prev1 = init_words.get(0), init_words.get(1)
   mut prev2, prev3 = init_words.get(2), init_words.get(3)
   mut r = 1
   while r < rounds {
      def g = _word_xor(_word_sub(_word_rot(prev3)), _rcon_word(r))
      def n0 = _word_xor(prev0, g)
      def n1 = _word_xor(prev1, n0)
      def n2 = _word_xor(prev2, n1)
      def n3 = _word_xor(prev3, n2)
      out = out.append(n0)
      out = out.append(n1)
      out = out.append(n2)
      out = out.append(n3)
      prev0 = n0
      prev1 = n1
      prev2 = n2
      prev3 = n3
      r += 1
   }
   out
}

fn aes_square_reverse_key_expand(list last_round_words, int rounds) list {
   "Rebuild the full AES-128 key schedule from a known final round key.
   last_round_words: 4 words of the last known round key
   rounds: number of round keys in the target schedule.
   Returns words from round-0 to round-(rounds-1)."
   error.crypto_require(rounds >= 1, "block.aes_square", "rounds must be >= 1")
   error.crypto_require_len(last_round_words, 4, "block.aes_square", "last_round_words")
   mut current = [
      clone(last_round_words.get(0, [])),
      clone(last_round_words.get(1, [])),
      clone(last_round_words.get(2, [])),
      clone(last_round_words.get(3, []))
   ]
   mut rev_keys = [current]
   mut r = rounds - 1
   while r >= 1 {
      def k0, k1 = current.get(0, []), current.get(1, [])
      def k2, k3 = current.get(2, []), current.get(3, [])
      def p3, p2 = _word_xor(k3, k2), _word_xor(k2, k1)
      def p1 = _word_xor(k1, k0)
      def g = _word_xor(_word_sub(_word_rot(p3)), _rcon_word(r))
      def p0 = _word_xor(k0, g)
      current = [p0, p1, p2, p3]
      rev_keys = rev_keys.append(current)
      r -= 1
   }
   mut out = []
   mut i = rev_keys.len - 1
   while i >= 0 {
      def rk = rev_keys.get(i, [])
      out = out.append(rk.get(0, []))
      out = out.append(rk.get(1, []))
      out = out.append(rk.get(2, []))
      out = out.append(rk.get(3, []))
      i -= 1
   }
   out
}

fn aes_square_recover_first_key(list last_round_key, int rounds) list {
   "Recover the initial AES-128 key from a later round key.
   last_round_key: 16-byte key material for round(rounds-1)
   rounds: number of round keys in schedule(for 4-round AES attack use 5)."
   def words = _key_to_words(last_round_key)
   def full_words = aes_square_reverse_key_expand(words, rounds)
   _words_to_key(slice(full_words, 0, 4))
}

fn aes_square_sub_bytes(list state) list {
   "Apply AES SubBytes on a 16-byte column-major state."
   error.crypto_require_len(state, 16, "block.aes_square", "state")
   mut out = []
   mut i = 0
   while i < 16 {
      out = out.append(_SBOX.get(state.get(i, 0) & 255, 0))
      i += 1
   }
   out
}

fn aes_square_shift_rows(list state) list {
   "Apply AES ShiftRows on a 16-byte column-major state."
   error.crypto_require_len(state, 16, "block.aes_square", "state")
   mut out = []
   mut c = 0
   while c < 4 {
      mut r = 0
      while r < 4 {
         def src_col = (c + r) % 4
         def src_idx = src_col * 4 + r
         out = out.append(state.get(src_idx, 0) & 255)
         r += 1
      }
      c += 1
   }
   out
}

fn aes_square_mix_columns(list state) list {
   "Apply AES MixColumns on a 16-byte column-major state."
   error.crypto_require_len(state, 16, "block.aes_square", "state")
   mut out = []
   mut c = 0
   while c < 4 {
      def b0, b1 = state.get(c * 4 + 0, 0) & 255, state.get(c * 4 + 1, 0) & 255
      def b2, b3 = state.get(c * 4 + 2, 0) & 255, state.get(c * 4 + 3, 0) & 255
      out = out.append((_gf_mul2(b0) ^^ _gf_mul3(b1) ^^ b2 ^^ b3) & 255)
      out = out.append((b0 ^^ _gf_mul2(b1) ^^ _gf_mul3(b2) ^^ b3) & 255)
      out = out.append((b0 ^^ b1 ^^ _gf_mul2(b2) ^^ _gf_mul3(b3)) & 255)
      out = out.append((_gf_mul3(b0) ^^ b1 ^^ b2 ^^ _gf_mul2(b3)) & 255)
      c += 1
   }
   out
}

fn aes_square_add_round_key(list state, list round_key_words) list {
   "XOR a state with one round key(4 words)."
   error.crypto_require_len(state, 16, "block.aes_square", "state")
   error.crypto_require_len(round_key_words, 4, "block.aes_square", "round_key_words")
   mut out = []
   mut c = 0
   while c < 4 {
      def w = round_key_words.get(c, [])
      error.crypto_require_len(w, 4, "block.aes_square", "round key word")
      mut r = 0
      while r < 4 {
         def idx = c * 4 + r
         out = out.append((state.get(idx, 0) ^^ w.get(r, 0)) & 255)
         r += 1
      }
      c += 1
   }
   out
}

fn aes_square_transform_state(list state, list key, int rounds=10) list {
   "Encrypt one state with AES round operations and configurable number of rounds."
   error.crypto_require(rounds >= 1, "block.aes_square", "rounds must be >= 1")
   error.crypto_require_len(state, 16, "block.aes_square", "state")
   error.crypto_require_len(key, 16, "block.aes_square", "key")
   def key_words = aes_square_key_expand(key, rounds + 1)
   mut cur = aes_square_add_round_key(state, slice(key_words, 0, 4))
   mut r = 1
   while r < rounds {
      def round_key = slice(key_words, 4 * r, 4 * (r + 1))
      cur = aes_square_sub_bytes(cur)
      cur = aes_square_shift_rows(cur)
      cur = aes_square_mix_columns(cur)
      cur = aes_square_add_round_key(cur, round_key)
      r += 1
   }
   cur = aes_square_sub_bytes(cur)
   cur = aes_square_shift_rows(cur)
   cur = aes_square_add_round_key(cur, slice(key_words, 4 * rounds, 4 * (rounds + 1)))
   cur
}

fn aes_square_encrypt_block(list plain_text, list key, int rounds=10) list {
   "Encrypt one 16-byte block with AES round logic.
   Uses 10 rounds by default(AES-128 full)."
   aes_square_transform_state(plain_text, key, rounds)
}

fn aes_square_state_from_rows(list rows) list {
   "Convert row-major 4x4 matrix into AES column-major 16-byte state."
   error.crypto_require_len(rows, 4, "block.aes_square", "rows")
   mut out = []
   mut c = 0
   while c < 4 {
      mut r = 0
      while r < 4 {
         def row = rows.get(r, [])
         error.crypto_require_len(row, 4, "block.aes_square", "row")
         out = out.append(row.get(c, 0) & 255)
         r += 1
      }
      c += 1
   }
   out
}

fn aes_square_state_to_rows(list state) list {
   "Convert AES column-major 16-byte state to row-major 4x4 matrix."
   error.crypto_require_len(state, 16, "block.aes_square", "state")
   [
      [state.get(0, 0), state.get(4, 0), state.get(8, 0), state.get(12, 0)],
      [state.get(1, 0), state.get(5, 0), state.get(9, 0), state.get(13, 0)],
      [state.get(2, 0), state.get(6, 0), state.get(10, 0), state.get(14, 0)],
      [state.get(3, 0), state.get(7, 0), state.get(11, 0), state.get(15, 0)]
   ]
}

fn aes_square_get_delta_set(int inactive_value) list {
   "Build the classic AES square-attack delta set(256 states).
   Byte 0 varies from 0..255, all other bytes are inactive_value."
   def iv = inactive_value & 255
   mut delta_set = []
   mut v = 0
   while v < 256 {
      mut state = [v & 255]
      mut i = 1
      while i < 16 {
         state = state.append(iv)
         i += 1
      }
      delta_set = delta_set.append(state)
      v += 1
   }
   delta_set
}

fn aes_square_encrypt_delta_set(list key, list delta_set, int rounds=4) list {
   "Encrypt every state in a delta set with a fixed AES key."
   mut out = []
   mut i = 0
   while i < delta_set.len {
      out = out.append(aes_square_transform_state(delta_set.get(i, []), key, rounds))
      i += 1
   }
   out
}

fn aes_square_gather_encrypted_delta_sets(fnptr encrypt_delta_set_fn) list {
   "Gather encrypted delta sets for inactive values 0x00..0x0f."
   mut encrypted = []
   mut inactive = 0
   while inactive < 16 {
      def ds = aes_square_get_delta_set(inactive)
      encrypted = encrypted.append(encrypt_delta_set_fn(ds))
      inactive += 1
   }
   encrypted
}

fn aes_square_reverse_state(int guess, int position, list encrypted_delta_set) list {
   "Reverse AddRoundKey+SubBytes for a guessed last-round key byte at one position."
   error.crypto_require(position >= 0 && position < 16, "block.aes_square", "position must be in [0,15]")
   mut reversed = []
   mut i = 0
   while i < encrypted_delta_set.len {
      def s = encrypted_delta_set.get(i, [])
      error.crypto_require_len(s, 16, "block.aes_square", "state")
      def before_add = (s.get(position, 0) ^^ guess) & 255
      reversed = reversed.append(_INV_SBOX.get(before_add, 0))
      i += 1
   }
   reversed
}

fn aes_square_is_guess_correct(list reversed_bytes) bool {
   "Integral property check: XOR of reversed bytes must be zero."
   mut x, i = 0, 0
   while i < reversed_bytes.len {
      x = (x ^^ (reversed_bytes.get(i, 0) & 255)) & 255
      i += 1
   }
   x == 0
}

fn aes_square_guess_position(list encrypted_delta_sets, int position) int {
   "Guess one byte of the last round key for a fixed byte position."
   error.crypto_require(position >= 0 && position < 16, "block.aes_square", "position must be in [0,15]")
   mut ds_i = 0
   while ds_i < encrypted_delta_sets.len {
      def encrypted_ds = encrypted_delta_sets.get(ds_i, [])
      mut candidates = []
      mut guess = 0
      while guess < 256 {
         def reversed = aes_square_reverse_state(guess, position, encrypted_ds)
         if aes_square_is_guess_correct(reversed) { candidates = candidates.append(guess) }
         guess += 1
      }
      if candidates.len == 1 { return candidates.get(0, 0) }
      ds_i += 1
   }
   error.crypto_fail("block.aes_square", "could not determine key byte at position " + str(position))
}

fn aes_square_crack_last_key(fnptr encrypt_delta_set_fn) list {
   "Recover the 16-byte last round key of a 4-round AES instance."
   def encrypted_sets = aes_square_gather_encrypted_delta_sets(encrypt_delta_set_fn)
   mut last_key = []
   mut pos = 0
   while pos < 16 {
      last_key = last_key.append(aes_square_guess_position(encrypted_sets, pos))
      pos += 1
   }
   last_key
}

fn aes_square_crack_key(fnptr encrypt_delta_set_fn, int rounds=4) list {
   "Recover the original AES key from a chosen-plaintext square attack."
   def last_key = aes_square_crack_last_key(encrypt_delta_set_fn)
   aes_square_recover_first_key(last_key, rounds + 1)
}
