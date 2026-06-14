;; Keywords: cipher atbash math crypto
;; Classical cipher routines for Atbash text transformation.
;; References:
;; - std.math.crypto.cipher
;; - std.math.crypto.analysis
module std.math.crypto.cipher.atbash(atbash_char, atbash_text)
use std.core
use std.core.str

fn atbash_char(str ch) str {
   "Mirror a single ASCII alphabetic character through the Atbash mapping."
   def code = ord(ch)
   if code >= 65 && code <= 90 { return chr(90 - (code - 65)) }
   if code >= 97 && code <= 122 { return chr(122 - (code - 97)) }
   ch
}

fn atbash_text(str s) str {
   "Apply Atbash to a whole string, preserving non-letters."
   mut out = Builder(s.len + 8)
   mut i = 0
   while i < s.len {
      out = builder_append(out, atbash_char(utf8_slice(s, i, i + 1, 1)))
      i += 1
   }
   def text = builder_to_str(out)
   builder_free(out)
   text
}
