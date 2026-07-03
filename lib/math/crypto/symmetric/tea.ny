;; Keywords: symmetric tea math crypto
;; Symmetric-crypto routines for TEA block cipher and equivalent-key analysis.
;; Reference:
;; - https://www.cl.cam.ac.uk/ftp/papers/djw-rmn/djw-rmn-tea.html
;; References:
;; - std.math.crypto.symmetric
;; - std.math.crypto
module std.math.crypto.symmetric.tea(tea_encrypt_block, tea_decrypt_block, tea_equivalent_keys)
use std.core
use std.math.bin (unpack_be32)
use std.math.crypto.error

fn _tea_u32(any x) int { int(x) & 0xffffffff }

fn _tea_word_bytes(any w) list {
   def x = _tea_u32(w)
   [(x >> 24) & 255, (x >> 16) & 255, (x >> 8) & 255, x & 255]
}

fn _tea_key_words(list key) list {
   crypto_require_len(key, 16, "symmetric.tea", "key")
   [unpack_be32(key, 0), unpack_be32(key, 4), unpack_be32(key, 8), unpack_be32(key, 12)]
}

fn tea_encrypt_block(list key, list block) list {
   "Encrypt one 8-byte TEA block using a 16-byte key."
   crypto_require_len(block, 8, "symmetric.tea_encrypt_block", "block")
   def k = _tea_key_words(key)
   mut v0, v1 = unpack_be32(block, 0), unpack_be32(block, 4)
   mut sum = 0
   def delta = 0x9e3779b9
   mut i = 0
   while i < 32 {
      sum = _tea_u32(sum + delta)
      v0 = _tea_u32(v0 + (((v1 << 4) + k[0]) ^^ (v1 + sum) ^^ ((v1 >> 5) + k[1])))
      v1 = _tea_u32(v1 + (((v0 << 4) + k[2]) ^^ (v0 + sum) ^^ ((v0 >> 5) + k[3])))
      i += 1
   }
   _tea_word_bytes(v0) + _tea_word_bytes(v1)
}

fn tea_decrypt_block(list key, list block) list {
   "Decrypt one 8-byte TEA block using a 16-byte key."
   crypto_require_len(block, 8, "symmetric.tea_decrypt_block", "block")
   def k = _tea_key_words(key)
   mut v0, v1 = unpack_be32(block, 0), unpack_be32(block, 4)
   def delta = 0x9e3779b9
   mut sum = _tea_u32(delta * 32)
   mut i = 0
   while i < 32 {
      v1 = _tea_u32(v1 - (((v0 << 4) + k[2]) ^^ (v0 + sum) ^^ ((v0 >> 5) + k[3])))
      v0 = _tea_u32(v0 - (((v1 << 4) + k[0]) ^^ (v1 + sum) ^^ ((v1 >> 5) + k[1])))
      sum = _tea_u32(sum - delta)
      i += 1
   }
   _tea_word_bytes(v0) + _tea_word_bytes(v1)
}

fn _tea_xor_80000000(list word) list {
   [word[0] ^^ 0x80, word[1], word[2], word[3]]
}

fn tea_equivalent_keys(list key) list {
   "Return TEA's four equivalent 128-bit keys.
   Flipping the high bit of both first-half words and/or both second-half words
   gives keys with identical encryption behavior."
   crypto_require_len(key, 16, "symmetric.tea_equivalent_keys", "key")
   def k0, k1 = slice(key, 0, 4), slice(key, 4, 8)
   def k2, k3 = slice(key, 8, 12), slice(key, 12, 16)
   [
      k0 + k1 + k2 + k3,
      k0 + k1 + _tea_xor_80000000(k2) + _tea_xor_80000000(k3),
      _tea_xor_80000000(k0) + _tea_xor_80000000(k1) + k2 + k3,
      _tea_xor_80000000(k0) + _tea_xor_80000000(k1) + _tea_xor_80000000(k2) + _tea_xor_80000000(k3),
   ]
}
