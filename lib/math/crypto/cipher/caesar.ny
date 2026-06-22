;; Keywords: cipher caesar math crypto
;; Caesar cipher encryption, decryption, and brute force routines.
;; Reference:
;; - https://netlab.cs.ucla.edu/wiki/files/shannon1949.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap1.pdf
;; References:
;; - std.math.crypto.cipher
;; - std.math.crypto.analysis
module std.math.crypto.cipher.caesar(caesar_encrypt, caesar_decrypt, caesar_bruteforce, caesar_encrypt_alnum, caesar_decrypt_alnum, caesar_progressive_encrypt, caesar_progressive_decrypt)
use std.core
use std.core.str
use std.math.crypto.error

fn _caesar_builder_take(list b) str { def out = builder_to_str(b) builder_free(b) out }

fn _caesar_apply_shift(int c, int shift, bool digits=false) str {
   case c {
      65..90 -> chr(((c - 65 + shift) % 26 + 26) % 26 + 65)
      97..122 -> chr(((c - 97 + shift) % 26 + 26) % 26 + 97)
      48..57 -> digits ? chr(((c - 48 + shift) % 10 + 10) % 10 + 48) : chr(c)
      _ -> chr(c)
   }
}

fn caesar_encrypt(str text, int shift) str {
   "Encrypt text using Caesar cipher with the given shift, preserving case and non-alphabetic characters."
   crypto_require(text != nil, "cipher.caesar_encrypt", "text is nil")
   mut n = text.len
   mut result = Builder(n + 8)
   mut i = 0
   while i < n {
      def c = load8(text, i)
      result = builder_append(result, _caesar_apply_shift(c, shift))
      i += 1
   }
   _caesar_builder_take(result)
}

fn caesar_decrypt(str text, int shift) str {
   "Decrypt text encrypted with Caesar cipher by reversing the shift."
   crypto_require(text != nil, "cipher.caesar_decrypt", "text is nil")
   caesar_encrypt(text, 0 - shift)
}

fn caesar_encrypt_alnum(str text, int shift) str {
   "Encrypt text using Caesar shift over letters and decimal digits."
   crypto_require(text != nil, "cipher.caesar_encrypt_alnum", "text is nil")
   mut result = Builder(text.len + 8)
   mut i = 0
   while i < text.len {
      result = builder_append(result, _caesar_apply_shift(load8(text, i), shift, true))
      i += 1
   }
   _caesar_builder_take(result)
}

fn caesar_decrypt_alnum(str text, int shift) str {
   "Decrypt a Caesar shift over letters and decimal digits."
   crypto_require(text != nil, "cipher.caesar_decrypt_alnum", "text is nil")
   caesar_encrypt_alnum(text, 0 - shift)
}

fn caesar_bruteforce(str text) list {
   "Try all 26 possible shifts and return a list of [shift, decrypted_text] pairs."
   crypto_require_nonempty(text, "cipher.caesar_bruteforce", "text")
   mut results = list(0)
   mut s = 0
   while s < 26 {
      def decrypted = caesar_decrypt(text, s)
      results = results.append([s, decrypted])
      s += 1
   }
   results
}

fn _caesar_progressive_apply(str text, int start_shift, bool step_all, int direction) str {
   crypto_require(text != nil, "cipher.caesar_progressive", "text is nil")
   mut result = Builder(text.len + 8)
   mut shift = start_shift
   mut i = 0
   while i < text.len {
      def c = load8(text, i)
      def is_alpha = (c >= 65 && c <= 90) || (c >= 97 && c <= 122)
      result = builder_append(result, _caesar_apply_shift(c, direction * shift))
      if is_alpha || step_all { shift += 1 }
      i += 1
   }
   _caesar_builder_take(result)
}

fn caesar_progressive_encrypt(str text, int start_shift=0, bool step_all=false) str {
   "Encrypt with a progressive Caesar/Trithemius shift.
   start_shift is used for the first byte. The shift increases after each
   alphabetic character ; when step_all is true, punctuation and digits also
   advance the shift."
   _caesar_progressive_apply(text, start_shift, step_all, 1)
}

fn caesar_progressive_decrypt(str text, int start_shift=0, bool step_all=false) str {
   "Decrypt text encrypted by caesar_progressive_encrypt."
   _caesar_progressive_apply(text, start_shift, step_all, -1)
}

#main {
   def msg = "Abc XyZ! 123"
   def enc = caesar_encrypt(msg, 2)
   assert(enc == "Cde ZaB! 123", "caesar encrypt preserves match and punctuation")
   assert(caesar_decrypt(enc, 2) == msg, "caesar decrypt reverses encrypt")
   assert(caesar_encrypt("Az", -1) == "Zy", "caesar negative shift wraps")
   def all = caesar_bruteforce("B")
   assert(all.len == 26, "caesar bruteforce returns all shifts")
   assert(all[0] == [0, "B"], "caesar bruteforce shift zero")
   assert(all[1] == [1, "A"], "caesar bruteforce shift one")
   def plain = "hello world this is a test"
   def cipher = caesar_progressive_encrypt(plain, 5, true)
   assert(cipher == "mkstx haezs kzbm ep z ugvx", "progressive caesar step-all encrypt")
   assert(caesar_progressive_decrypt(cipher, 5, true) == plain, "progressive caesar step-all decrypt")
   assert(caesar_progressive_encrypt("abc-xyz", 0, false) == "ace-ace", "progressive caesar alpha-only")
   assert(caesar_progressive_decrypt("ace-ace", 0, false) == "abc-xyz", "progressive caesar alpha-only decrypt")
   print("✓ std.math.crypto.cipher.caesar self-test passed")
}
