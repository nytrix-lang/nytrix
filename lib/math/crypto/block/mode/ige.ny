;; Keywords: block-cipher mode ige math crypto
;; Block-mode routines for IGE bit-flipping analysis.
;; References:
;; - std.math.crypto.block.mode
;; - std.math.crypto
module std.math.crypto.block.mode.ige(ige_bit_flipping)
use std.core
use std.math.bin

fn ige_bit_flipping(any iv1, any iv2, list ct, int pos, int old_byte, int new_byte) list {
   "Flip a byte in IGE-decrypted plaintext by modifying ciphertext bytes.
   IGE mode decryption:
   P[i] = D(C[i]) XOR C[i-1]    (with C[0] = iv2 for the first block)
   C[i] = E(P[i] XOR C[i-1]) XOR C[i-2]  (encryption direction)
   For bit flipping in IGE, modifying C[j] affects P[j] and garbles P[j+1].
   iv1: first IV(used in encryption chaining)
   iv2: second IV(C[-1] equivalent for first block decryption)
   ct: ciphertext as byte list
   pos: byte position in decrypted plaintext to modify
   old_byte: current byte value at that position
   new_byte: desired byte value after modification
   Returns modified ciphertext byte list."
   def block_size = 16
   def diff = bxor(old_byte, new_byte)
   def block_idx = pos / block_size
   def byte_offset = pos % block_size
   mut modified_ct = clone(ct)
   def ct_block = block_idx + 1
   def target_pos = (ct_block - 1) * block_size + byte_offset
   def ct_len = ct.len
   if(target_pos < ct_len){
      def orig_ct_byte = modified_ct[target_pos]
      modified_ct[target_pos] = bxor(orig_ct_byte, diff)
   }
   if(ct_block < (ct_len / block_size)){
      def next_block_start = ct_block * block_size + byte_offset
      def prev_next_byte = modified_ct[next_block_start]
      modified_ct[next_block_start] = bxor(prev_next_byte, diff)
   }
   modified_ct
}
