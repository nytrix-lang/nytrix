;; Keywords: block-cipher mode tools
;; Block mode utilities.
module std.math.crypto.block.mode.tools(detect_block_mode, recover_cbc_iv, cbc_bitflip_delta, cbc_apply_bitflip_delta, addition_block_chaining_roll, addition_block_chaining_unroll)
use std.core
use std.math.integer (Z)
use std.math.crypto.block.mode.ecb
use std.math.crypto.support.tools as support

fn _xor_prefix(any: a, any: b): list {
   def n = (a.len < b.len) ? a.len : b.len
   mut out = list(n)
   mut i = 0
   while(i < n){
      out = out.append(a[i] ^^ b[i])
      i += 1
   }
   out
}

fn cbc_bitflip_delta(any: original_plain, any: target_plain): any {
   "Return the XOR delta needed in the previous CBC block to turn original_plain into target_plain."
   (original_plain == nil || target_plain == nil) ? nil : _xor_prefix(original_plain, target_plain)
}

fn cbc_apply_bitflip_delta(any: previous_cipher_block, any: delta): any {
   "Apply a CBC bitflip delta to the previous ciphertext block."
   if(previous_cipher_block == nil || delta == nil){ return nil }
   mut out = _xor_prefix(previous_cipher_block, delta)
   mut i = out.len
   while(i < previous_cipher_block.len){
      out = out.append(previous_cipher_block[i])
      i += 1
   }
   out
}

fn detect_block_mode(any: ciphertext, int: block_size): str {
   "Heuristic block-mode detector.
   Returns one of: ecb, cbc_or_ctr, too_short."
   if(ciphertext == nil || ciphertext.len < block_size * 2){ return "too_short" }
   ecb_detect(ciphertext, block_size) > 0 ? "ecb" : "cbc_or_ctr"
}

fn _abc_block_int(list: data, int: off, int: block_size): any {
   slice(data, off, off + block_size).long
}

fn _abc_append_fixed(list: out, any: value, int: block_size): list {
   out.extend(support.bytes_fixed_from_bigint(value, block_size))
}

fn addition_block_chaining_roll(list: ecb_ciphertext, list: iv, int: block_size=16): any {
   "Apply Addition Block Chaining post-processing to ECB blocks.
   Returns iv || abc_blocks, where each block is added to the previous block
   modulo 256^block_size."
   if(ecb_ciphertext == nil || iv == nil || block_size <= 0){ return nil }
   if(iv.len != block_size || ecb_ciphertext.len % block_size != 0){ return nil }
   def modulus = Z(1) << (block_size * 8)
   mut out = clone(iv)
   mut prev = iv.long
   mut off = 0
   while(off < ecb_ciphertext.len){
      def cur = _abc_block_int(ecb_ciphertext, off, block_size)
      def chained = (prev + cur) % modulus
      out = _abc_append_fixed(out, chained, block_size)
      prev = chained
      off += block_size
   }
   out
}

fn addition_block_chaining_unroll(list: abc_ciphertext, int: block_size=16): any {
   "Undo Addition Block Chaining post-processing.
   Input must be iv || abc_blocks. Returns the original ECB-shaped blocks."
   if(abc_ciphertext == nil || block_size <= 0){ return nil }
   if(abc_ciphertext.len < block_size || abc_ciphertext.len % block_size != 0){ return nil }
   def modulus = Z(1) << (block_size * 8)
   mut out = []
   mut prev = _abc_block_int(abc_ciphertext, 0, block_size)
   mut off = block_size
   while(off < abc_ciphertext.len){
      def cur = _abc_block_int(abc_ciphertext, off, block_size)
      def raw = (cur - prev + modulus) % modulus
      out = _abc_append_fixed(out, raw, block_size)
      prev = cur
      off += block_size
   }
   out
}

fn recover_cbc_iv(fnptr: decrypt_oracle_fn, any: ciphertext, int: block_size): any {
   "Recover CBC IV from a raw decryption oracle by querying c1 || 0^b || c1.
   decrypt_oracle_fn(ct) must return the raw CBC-decrypted plaintext bytes."
   if(ciphertext == nil || ciphertext.len < block_size){ return nil }
   mut c1 = []
   mut i = 0
   while(i < block_size){
      c1 = c1.append(ciphertext[i])
      i += 1
   }
   mut query = clone(c1)
   i = 0
   while(i < block_size){
      query = query.append(0)
      i += 1
   }
   i = 0
   while(i < block_size){
      query = query.append(c1[i])
      i += 1
   }
   def pt = decrypt_oracle_fn(query)
   if(pt == nil || pt.len < block_size * 3){ return nil }
   mut iv = []
   i = 0
   while(i < block_size){
      iv = iv.append(pt[i] ^^ pt[block_size * 2 + i])
      i += 1
   }
   iv
}
