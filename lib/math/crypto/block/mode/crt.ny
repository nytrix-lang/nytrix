;; Keywords: block-cipher mode crt
;; Chinese-remainder block recombination for shared-message modulus attacks.
;; Reference:
;; - https://cacr.uwaterloo.ca/hac/about/chap2.pdf
module std.math.crypto.block.mode.crt(crt_block_combine)
use std.core
use std.math.nt
use std.math.bin

fn crt_block_combine(list: remainders, list: moduli, int: block_size): list {
   "Combine partial decryptions from multiple RSA/block-cipher moduli using CRT.
   Given ciphertexts encrypted under different moduli with the same small exponent,
   recover the original plaintext via the Chinese Remainder Theorem.
   remainders: list of byte lists(ciphertext remainders from each modulus)
   moduli: list of integers(the moduli for each remainder)
   block_size: target block size for output
   Returns the combined plaintext as a byte list."
   mut n = remainders.len
   if(n == 0){ return list(0) }
   if(n == 1){ return clone(remainders[0]) }
   def num_coeffs = moduli.len
   mut int_rems = list(0)
   mut i = 0
   while(i < n){
      def rem_bytes = remainders[i]
      mut val = _bytes_to_int_le(rem_bytes)
      int_rems = int_rems.append(val)
      i += 1
   }
   mut int_mods = list(0)
   i = 0
   while(i < num_coeffs){
      int_mods = int_mods.append(moduli[i])
      i += 1
   }
   mut result = crt(int_rems, int_mods)
   _int_to_bytes_le(result, block_size)
}

fn _bytes_to_int_le(list: bytes): any {
   "Convert a byte list to an integer(little-endian).
   bytes: byte list
   Returns the integer value."
   mut n = bytes.len
   mut result = 0
   mut shift = 0
   mut i = 0
   while(i < n){
      result = result + (bytes[i] << shift)
      shift = shift + 8
      i += 1
   }
   result
}

fn _int_to_bytes_le(any: value, int: block_size): list {
   "Convert an integer to a byte list(little-endian, padded to block_size).
   value: integer to convert
   block_size: number of bytes in output
   Returns the byte list."
   mut result = list(0)
   mut v = value
   mut i = 0
   while(i < block_size){
      result = result.append(v & 255)
      v = v >> 8
      i += 1
   }
   result
}
