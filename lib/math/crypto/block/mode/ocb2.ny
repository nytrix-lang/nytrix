;; Keywords: block-cipher mode ocb2 math crypto
;; Block-mode routines for OCB2 known-plaintext forgery.
;; Reference:
;; - Phillip Rogaway, "Efficient Instantiations of Tweakable Blockciphers
;;   and Refinements to Modes OCB and PMAC"
;;   http://web.cs.ucdavis.edu/~rogaway/papers/offsets.pdf
;;
;; OCB2 is a real authenticated-encryption mode. The original proof had a
;; flaw; with one encryption-oracle answer for a carefully chosen two-block
;; plaintext, an adversary can forge a valid one-block ciphertext/tag pair.
;; References:
;; - std.math.crypto.block.mode
;; - std.math.crypto
module std.math.crypto.block.mode.ocb2(ocb2_known_plaintext_two_block_forgery)
use std.core

fn _ocb2_xor_min(list a, list b) list {
   def n = (a.len < b.len) ? a.len : b.len
   mut out = []
   mut i = 0
   while i < n {
      out = out.append(a[i] ^^ b[i])
      i += 1
   }
   out
}

fn ocb2_known_plaintext_two_block_forgery(list plaintext, list ciphertext) any {
   "Forge the classic OCB2 one-block ciphertext/tag pair from a known
   two-block plaintext/ciphertext oracle response.
   plaintext: the two-block plaintext sent to the encryption oracle.
   ciphertext: the two-block ciphertext returned by the oracle.
   Returns [forged_ciphertext, forged_tag]."
   if plaintext.len != ciphertext.len { return nil }
   if ciphertext.len % 2 != 0 { return nil }
   def n = ciphertext.len / 2
   def p0 = slice(plaintext, 0, n)
   def p1 = slice(plaintext, n, plaintext.len)
   def c1 = slice(ciphertext, n, ciphertext.len)
   [_ocb2_xor_min(p0, ciphertext), _ocb2_xor_min(p1, c1)]
}

#main {
   def plaintext = "000000000000000000000000000000804f7edcc5b51dbbbadf72b01ec6d626ba".unhex
   def ciphertext = "237ced456bc5d05b1003e95a56b5c42f8117a934e1158678157e06ea017e9eb3".unhex
   def forged = ocb2_known_plaintext_two_block_forgery(plaintext, ciphertext)
   assert(forged != nil, "ocb2 forgery returned result")
   assert(forged.get(0) == [35, 124, 237, 69, 107, 197, 208, 91, 16, 3, 233, 90, 86, 181, 196, 175], "ocb2 forged ciphertext")
   assert(forged.get(1) == [206, 105, 117, 241, 84, 8, 61, 194, 202, 12, 182, 244, 199, 168, 184, 9], "ocb2 forged tag")
   assert(ocb2_known_plaintext_two_block_forgery([1], [1, 2]) == nil, "ocb2 rejects mismatched length")
   print("✓ std.math.crypto.block.mode.ocb2 self-test passed")
}
