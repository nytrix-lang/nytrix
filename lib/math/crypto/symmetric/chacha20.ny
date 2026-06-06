;; Keywords: symmetric chacha20 math crypto
;; Symmetric-crypto routines for ChaCha20 stream cipher operations.
;; Reference: RFC 8439 (https://tools.ietf.org/html/rfc8439)
;; References:
;; - std.math.crypto.symmetric
;; - std.math.crypto
module std.math.crypto.symmetric.chacha20(chacha20_encrypt, chacha20_decrypt, chacha20_encrypt_64nonce, chacha20_decrypt_64nonce, chacha20_block)
use std.core
use std.math.bin (zero_list, unpack_le32)

fn _rotl32(int x, int n) int { ((x << n) | (x >> (32 - n))) & 0xffffffff }

fn _quarter_round(list st, int a, int b, int c, int d) any {
   mut va, vb, vc, vd = st[a], st[b], st[c], st[d]
   va = (va + vb) & 0xffffffff
   vd = _rotl32(vd ^^ va, 16)
   vc = (vc + vd) & 0xffffffff
   vb = _rotl32(vb ^^ vc, 12)
   va = (va + vb) & 0xffffffff
   vd = _rotl32(vd ^^ va, 8)
   vc = (vc + vd) & 0xffffffff
   vb = _rotl32(vb ^^ vc, 7)
   st[a] = va st[b] = vb
   st[c] = vc st[d] = vd
}

fn chacha20_block(list st) list {
   "Inner block function: 20 rounds(10 column rounds, 10 diagonal rounds)."
   mut st_orig = clone(st)
   _chacha20_block_with_orig(st, st_orig)
}

fn _chacha20_block_with_orig(list st, list st_orig) list {
   mut i = 0
   while(i < 10){
      _quarter_round(st, 0, 4, 8, 12)
      _quarter_round(st, 1, 5, 9, 13)
      _quarter_round(st, 2, 6, 10, 14)
      _quarter_round(st, 3, 7, 11, 15)
      _quarter_round(st, 0, 5, 10, 15)
      _quarter_round(st, 1, 6, 11, 12)
      _quarter_round(st, 2, 7, 8, 13)
      _quarter_round(st, 3, 4, 9, 14)
      i += 1
   }
   mut j = 0 while(j < 16){
      st[j] = (st[j] + st_orig[j]) & 0xffffffff
      j += 1
   }
   st
}

fn _chacha20_base_ctx(list key) list {
   mut ctx = zero_list(16)
   ctx[0] = 0x61707865 ctx[1] = 0x3320646e
   ctx[2] = 0x79622d32 ctx[3] = 0x6b206574
   mut i = 0 while(i < 8){
      ctx[4 + i] = unpack_le32(key, i * 4)
      i += 1
   }
   ctx
}

fn _chacha20_xor_words(list out, list data, int offset, list st) list {
   mut wi = 0
   mut j = 0
   def remaining = data.len - offset
   while(wi < 16 && j < remaining){
      def w = st[wi]
      out[offset + j] = data[offset + j] ^^ (w & 0xff)
      if(j + 1 < remaining){ out[offset + j + 1] = data[offset + j + 1] ^^ ((w >> 8) & 0xff) }
      if(j + 2 < remaining){ out[offset + j + 2] = data[offset + j + 2] ^^ ((w >> 16) & 0xff) }
      if(j + 3 < remaining){ out[offset + j + 3] = data[offset + j + 3] ^^ ((w >> 24) & 0xff) }
      wi += 1
      j += 4
   }
   out
}

fn _chacha20_crypt_ctx(list ctx, int counter, list data, bool wide_counter) list {
   mut res = list(data.len)
   store64(res, data.len, 0)
   mut st = zero_list(16)
   mut st_orig = zero_list(16)
   mut p = 0
   mut block_counter = counter
   while(p < data.len){
      mut i = 0
      while(i < 16){
         def v = ctx[i]
         st[i] = v
         st_orig[i] = v
         i += 1
      }
      st[12] = wide_counter ? (block_counter & 0xffffffff) : block_counter
      st_orig[12] = st[12]
      if(wide_counter){ st[13] = (block_counter >> 32) & 0xffffffff }
      if(wide_counter){ st_orig[13] = st[13] }
      _chacha20_block_with_orig(st, st_orig)
      _chacha20_xor_words(res, data, p, st)
      p += 64
      block_counter += 1
   }
   res
}

fn chacha20_encrypt(list key, list nonce, int counter, list plaintext) list {
   "Encrypt plaintext with ChaCha20."
   mut ctx = _chacha20_base_ctx(key)
   ctx[12] = counter
   ctx[13] = unpack_le32(nonce, 0)
   ctx[14] = unpack_le32(nonce, 4)
   ctx[15] = unpack_le32(nonce, 8)
   _chacha20_crypt_ctx(ctx, counter, plaintext, false)
}

fn chacha20_decrypt(list key, list nonce, int counter, list ciphertext) list {
   "Decrypt with IETF ChaCha20; encryption and decryption are identical."
   chacha20_encrypt(key, nonce, counter, ciphertext)
}

fn chacha20_encrypt_64nonce(list key, list nonce, int counter, list plaintext) list {
   "Encrypt with original ChaCha20 layout: 64-bit block counter and 64-bit nonce.
   This matches PyCryptodome's ChaCha20 mode when an 8-byte nonce is supplied."
   mut ctx = _chacha20_base_ctx(key)
   ctx[12] = counter & 0xffffffff
   ctx[13] = (counter >> 32) & 0xffffffff
   ctx[14] = unpack_le32(nonce, 0)
   ctx[15] = unpack_le32(nonce, 4)
   _chacha20_crypt_ctx(ctx, counter, plaintext, true)
}

fn chacha20_decrypt_64nonce(list key, list nonce, int counter, list ciphertext) list {
   "Decrypt original 64-bit-nonce ChaCha20; encryption and decryption are identical."
   chacha20_encrypt_64nonce(key, nonce, counter, ciphertext)
}
