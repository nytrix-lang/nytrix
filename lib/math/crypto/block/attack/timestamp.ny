;; Keywords: block-cipher attack timestamp math crypto
;; Block-cipher attack routines for timestamp-key brute force.
;; References:
;; - std.math.crypto.block.attack
;; - std.math.crypto
module std.math.crypto.block.attack.timestamp(aes_ecb_sha256_timestamp_bruteforce)
use std.core
use std.core.str as str
use std.math.bin as bin
use std.math.crypto.hash (sha256)
use std.math.crypto.symmetric.aes (aes_decrypt_ecb)

fn aes_ecb_sha256_timestamp_bruteforce(list ciphertext, int center, int radius, str prefix="", str suffix="") any {
   "Try AES-ECB keys sha256(str(timestamp))[:16] in [center-radius, center+radius].
   Returns [timestamp, plaintext] when optional prefix/suffix checks match, else nil."
   mut timestamp = center - radius
   def stop = center + radius
   while timestamp <= stop {
      def key = slice(sha256(to_str(timestamp)), 0, 16)
      def padded = aes_decrypt_ecb(key, ciphertext)
      if padded != nil {
         def text = bin.pkcs7_unpad(padded).text
         def prefix_ok = prefix == "" || str.startswith(text, prefix)
         def suffix_ok = suffix == "" || str.endswith(text, suffix)
         if prefix_ok && suffix_ok { return [timestamp, text] }
      }
      timestamp += 1
   }
   nil
}
