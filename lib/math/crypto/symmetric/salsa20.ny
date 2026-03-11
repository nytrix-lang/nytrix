;; Keywords: symmetric salsa20
;; Symmetric-crypto routines for Salsa20 stream cipher operations.
;; Reference: https://cr.yp.to/snuffle/salsa20.pdf
module std.math.crypto.symmetric.salsa20(salsa20_encrypt, salsa20_decrypt, salsa20_block)
use std.core
use std.math.bin (unpack_le32)

fn _rotl32(int: x, int: n): int { ((x << n) | (x >> (32 - n))) & 0xffffffff }

fn _quarter_round(list: v, int: a, int: b, int: c, int: d): any {
   mut va, vb, vc, vd = v[a], v[b], v[c], v[d]
   vb = vb ^^ _rotl32((va + vd) & 0xffffffff, 7)
   vc = vc ^^ _rotl32((vb + va) & 0xffffffff, 9)
   vd = vd ^^ _rotl32((vc + vb) & 0xffffffff, 13)
   va = va ^^ _rotl32((vd + vc) & 0xffffffff, 18)
   v[a] = va v[b] = vb
   v[c] = vc v[d] = vd
}

fn salsa20_block(list: st): list {
   "Salsa20 core: 20 rounds."
   mut st_orig = clone(st)
   _salsa20_block_with_orig(st, st_orig)
}

fn _salsa20_block_with_orig(list: st, list: st_orig): list {
   mut i = 0
   while(i < 10){
      _quarter_round(st, 0, 4, 8, 12)
      _quarter_round(st, 5, 9, 13, 1)
      _quarter_round(st, 10, 14, 2, 6)
      _quarter_round(st, 15, 3, 7, 11)
      _quarter_round(st, 0, 1, 2, 3)
      _quarter_round(st, 5, 6, 7, 4)
      _quarter_round(st, 10, 11, 8, 9)
      _quarter_round(st, 15, 12, 13, 14)
      i += 1
   }
   mut j = 0 while(j < 16){
      st[j] = (st[j] + st_orig[j]) & 0xffffffff
      j += 1
   }
   st
}

fn _salsa20_base_ctx(list: key, list: nonce, int: counter): list {
   mut ctx = list(16)
   ctx[0] = 0x61707865 ctx[5] = 0x3320646e
   ctx[10] = 0x79622d32 ctx[15] = 0x6b206574
   mut i = 0 while(i < 4){ ctx[1 + i] = unpack_le32(key, i * 4) i += 1 }
   i = 0 while(i < 4){ ctx[11 + i] = unpack_le32(key, 16 + i * 4) i += 1 }
   ctx[6] = unpack_le32(nonce, 0)
   ctx[7] = unpack_le32(nonce, 4)
   ctx[8] = counter & 0xffffffff
   ctx[9] = (counter >> 32) & 0xffffffff
   ctx
}

fn _salsa20_xor_block(list: out, list: data, int: offset, list: st): list {
   mut j = 0
   while(j < 64 && (offset + j) < data.len){
      def keystream_word = st[j / 4]
      def keystream_byte = (keystream_word >> (8 * (j % 4))) & 0xff
      out[offset + j] = data[offset + j] ^^ keystream_byte
      j += 1
   }
   out
}

fn salsa20_encrypt(list: key, list: nonce, int: counter, list: plaintext): list {
   "Encrypt plaintext with Salsa20."
   mut ctx = _salsa20_base_ctx(key, nonce, counter)
   mut res = list(plaintext.len)
   store64(res, plaintext.len, 0)
   mut st = list(16)
   mut st_orig = list(16)
   store64(st, 16, 0)
   store64(st_orig, 16, 0)
   mut p = 0
   mut block_counter = counter
   while(p < plaintext.len){
      mut i = 0
      while(i < 16){
         def v = ctx[i]
         st[i] = v
         st_orig[i] = v
         i += 1
      }
      st[8] = block_counter & 0xffffffff
      st[9] = (block_counter >> 32) & 0xffffffff
      st_orig[8] = st[8]
      st_orig[9] = st[9]
      _salsa20_block_with_orig(st, st_orig)
      _salsa20_xor_block(res, plaintext, p, st)
      p += 64
      block_counter += 1
   }
   res
}

fn salsa20_decrypt(list: key, list: nonce, int: counter, list: ciphertext): list {
   "Decrypt with Salsa20; encryption and decryption are identical."
   salsa20_encrypt(key, nonce, counter, ciphertext)
}
