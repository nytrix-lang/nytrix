;; Keywords: cipher rail-fence
;; Rail-fence cipher encryption, decryption, and recovery routines.
;; Reference:
;; - https://netlab.cs.ucla.edu/wiki/files/shannon1949.pdf
;; - https://cacr.uwaterloo.ca/hac/about/chap1.pdf
module std.math.crypto.cipher.rail_fence(rail_fence_encrypt, rail_fence_decrypt, rail_fence_encrypt_offset, rail_fence_decrypt_offset, rail_fence_crack)
use std.core
use std.core.str
use std.math.crypto.error

fn _rail_builder_take(list: b): str { def out = builder_to_str(b) builder_free(b) out }

fn _rail_zero_list(int: n): list {
   mut out, i = list(0), 0
   while(i < n){ out = out.append(0) i += 1 }
   out
}

fn _rail_phase_row(int: phase, int: rails): int {
   def period = 2 * (rails - 1)
   mut p = phase % period
   if(p < 0){ p += period }
   if(p < rails){ return p }
   period - p
}

fn _rail_lengths_offset(int: n, int: rails, int: offset): list {
   mut lens = _rail_zero_list(rails)
   mut i = 0
   while(i < n){
      def row = _rail_phase_row(offset + i, rails)
      lens[row] = lens[row] + 1
      i += 1
   }
   lens
}

fn rail_fence_encrypt(str: text, int: rails): str {
   "Encrypt text using the rail fence cipher with the specified number of rails."
   rail_fence_encrypt_offset(text, rails, 0)
}

fn rail_fence_encrypt_offset(str: text, int: rails, int: offset=0): str {
   "Encrypt text using a rail fence whose zigzag starts at the given phase offset."
   crypto_require(text != nil, "cipher.rail_fence_encrypt_offset", "text is nil")
   crypto_require(rails != nil, "cipher.rail_fence_encrypt_offset", "rails is nil")
   mut n = text.len
   if(rails < 2 || n == 0){ return text }
   if(rails > n){ rails = n }
   mut fence, r = list(0), 0
   while(r < rails){ fence = fence.append(Builder((n / rails) + 8)) r += 1 }
   mut i = 0
   while(i < n){
      def row = _rail_phase_row(offset + i, rails)
      mut current = fence[row]
      current = builder_append(current, chr(load8(text, i)))
      fence[row] = current
      i += 1
   }
   mut result = Builder(n + 8)
   r = 0
   while(r < rails){
      def rail_b = fence[r]
      result = builder_append(result, builder_to_str(rail_b))
      builder_free(rail_b)
      r += 1
   }
   _rail_builder_take(result)
}

fn rail_fence_decrypt(str: ciphertext, int: rails): str {
   "Decrypt text encrypted with the rail fence cipher by reconstructing the zigzag rail pattern."
   rail_fence_decrypt_offset(ciphertext, rails, 0)
}

fn rail_fence_decrypt_offset(str: ciphertext, int: rails, int: offset=0): str {
   "Decrypt text encrypted with a rail fence whose zigzag starts at the given phase offset."
   crypto_require(ciphertext != nil, "cipher.rail_fence_decrypt_offset", "ciphertext is nil")
   crypto_require(rails != nil, "cipher.rail_fence_decrypt_offset", "rails is nil")
   mut n = ciphertext.len
   if(rails < 2 || n == 0){ return ciphertext }
   if(rails > n){ rails = n }
   def rail_lens = _rail_lengths_offset(n, rails, offset)
   mut fence, pos, r = list(0), 0, 0
   while(r < rails){
      def rl = rail_lens[r]
      mut segment = Builder(rl + 8)
      mut j = 0
      while(j < rl){ segment = builder_append(segment, chr(load8(ciphertext, pos))) pos += 1 j += 1 }
      fence = fence.append(_rail_builder_take(segment))
      r += 1
   }
   mut rail_indices = _rail_zero_list(rails)
   mut result = Builder(n + 8)
   mut i = 0
   while(i < n){
      def row = _rail_phase_row(offset + i, rails)
      def ri = rail_indices[row]
      def rail_str = fence[row]
      result = builder_append(result, chr(load8(rail_str, ri)))
      rail_indices[row] = ri + 1
      i += 1
   }
   _rail_builder_take(result)
}

fn rail_fence_crack(str: ciphertext): list {
   "Try all possible rail counts from 2 to length-1 and return a list of [rails, plaintext] pairs."
   crypto_require_nonempty(ciphertext, "cipher.rail_fence_crack", "ciphertext")
   mut n = ciphertext.len
   mut results = list(0)
   mut rails = 2
   while(rails < n){
      def plaintext = rail_fence_decrypt(ciphertext, rails)
      results = results.append([rails, plaintext])
      rails += 1
   }
   results
}
