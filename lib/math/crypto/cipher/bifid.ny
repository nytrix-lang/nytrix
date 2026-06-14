;; Keywords: cipher bifid math crypto
;; Bifid cipher decryption and CBC-style variants routines.
;; References:
;; - std.math.crypto.cipher
;; - std.math.crypto.analysis
module std.math.crypto.cipher.bifid(bifid_cbc_decrypt, bifid_cbc_decrypt_block, bifid_cbc_unmask)
use std.core
use std.core.str
use std.math.crypto.error

fn _bifid_ch(str s, int i) str {
   str_slice(s, i, i + 1, 1)
}

fn _bifid_index(str alphabet, str ch, str scope) int {
   def idx = find(alphabet, ch)
   if idx < 0 { crypto_fail(scope, "character not in alphabet: " + ch) }
   idx
}

fn bifid_cbc_unmask(str message, str mask, str alphabet="ABCDEFGHIKLMNOPQRSTUVWXYZ") str {
   "Undo the additive alphabet mask used by the CBC-style Bifid variant."
   crypto_require_nonempty(message, "cipher.bifid_cbc_unmask", "message")
   crypto_require_nonempty(mask, "cipher.bifid_cbc_unmask", "mask")
   def n = message.len
   mut out = ""
   mut i = 0
   while i < n {
      def a = _bifid_index(alphabet, _bifid_ch(message, i), "cipher.bifid_cbc_unmask")
      def b = _bifid_index(alphabet, _bifid_ch(mask, i % mask.len), "cipher.bifid_cbc_unmask")
      out += _bifid_ch(alphabet, (alphabet.len + a - b) % alphabet.len)
      i += 1
   }
   out
}

fn bifid_cbc_decrypt_block(str key, str ciphertext_block, int period=8) str {
   "Reverse one keyed 5x5 Bifid block before CBC unmasking."
   crypto_require_len(key, 25, "cipher.bifid_cbc_decrypt_block", "key")
   crypto_require_len(ciphertext_block, period, "cipher.bifid_cbc_decrypt_block", "ciphertext_block")
   mut out = ""
   mut i = 0
   while i < period {
      def a = _bifid_ch(ciphertext_block, i / 2)
      def b = _bifid_ch(ciphertext_block, (period + i) / 2)
      def ai = _bifid_index(key, a, "cipher.bifid_cbc_decrypt_block")
      def bi = _bifid_index(key, b, "cipher.bifid_cbc_decrypt_block")
      def ar, ac = ai / 5, ai % 5
      def br, bc = bi / 5, bi % 5
      out += (i % 2 == 0) ? _bifid_ch(key, 5 * ar + br) : _bifid_ch(key, 5 * ac + bc)
      i += 1
   }
   out
}

fn bifid_cbc_decrypt(str key, str iv, str ciphertext, int period=8) str {
   "Decrypt the HTB BFD56 CBC-style Bifid construction."
   crypto_require_len(iv, period, "cipher.bifid_cbc_decrypt", "iv")
   crypto_require(ciphertext.len % period == 0, "cipher.bifid_cbc_decrypt", "ciphertext length must be a multiple of period")
   mut out = ""
   mut mask = iv
   mut i = 0
   while i < ciphertext.len {
      def block = str_slice(ciphertext, i, i + period, 1)
      out += bifid_cbc_unmask(bifid_cbc_decrypt_block(key, block, period), mask)
      mask = block
      i += period
   }
   out
}
