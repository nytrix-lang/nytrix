;; Keywords: block-cipher mode gcm math crypto
;; Block-mode routines for GCM encryption, authentication, nonce-reuse recovery, and tag forgery.
;; Reference:
;; - https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication800-38d.pdf
;; - https://eprint.iacr.org/2012/453.pdf (Joux nonce reuse)
;; References:
;; - std.math.crypto.block.mode
;; - std.math.crypto
module std.math.crypto.block.mode.gcm(gcm_nonce_reuse_decrypt, gcm_recover_keystream, ghash, gcm_forge_tag, recover_e0, forge_tag_from_known, gcm_recover_ectr0_from_sample, gcm_forge_tag_from_known_message, gcm_recover_auth_key_one_block, gf128_mult, gf128_inv, gf128_sqrt, gcm_ghash, gcm_auth_ghash, gcm_encrypt, gcm_decrypt, gcm_verify_tag)
use std.core
use std.math.nt
use std.math.bin
use std.math.crypto.symmetric.aes

fn _gf128_one() list { [128,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0] }

fn _gf128_zero() list { [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0] }

fn _gcm_be32_at(list data, int off) int {
   (__load_item_fast(data, off) << 24) |
   (__load_item_fast(data, off + 1) << 16) |
   (__load_item_fast(data, off + 2) << 8) |
   __load_item_fast(data, off + 3)
}

fn _gcm_be32_at_padded(list data, int off) int {
   def n = len(data)
   ((off < n ? __load_item_fast(data, off) : 0) << 24) |
   ((off + 1 < n ? __load_item_fast(data, off + 1) : 0) << 16) |
   ((off + 2 < n ? __load_item_fast(data, off + 2) : 0) << 8) |
   (off + 3 < n ? __load_item_fast(data, off + 3) : 0)
}

fn _gf128_words(list x) list {
   [_gcm_be32_at(x, 0), _gcm_be32_at(x, 4), _gcm_be32_at(x, 8), _gcm_be32_at(x, 12)]
}

fn _gf128_words_to_bytes(int w0, int w1, int w2, int w3) list {
   mut out = []
   out = out.append((w0 >> 24) & 255)
   out = out.append((w0 >> 16) & 255)
   out = out.append((w0 >> 8) & 255)
   out = out.append(w0 & 255)
   out = out.append((w1 >> 24) & 255)
   out = out.append((w1 >> 16) & 255)
   out = out.append((w1 >> 8) & 255)
   out = out.append(w1 & 255)
   out = out.append((w2 >> 24) & 255)
   out = out.append((w2 >> 16) & 255)
   out = out.append((w2 >> 8) & 255)
   out = out.append(w2 & 255)
   out = out.append((w3 >> 24) & 255)
   out = out.append((w3 >> 16) & 255)
   out = out.append((w3 >> 8) & 255)
   out = out.append(w3 & 255)
   out
}

fn _gf128_mult_words(int x0, int x1, int x2, int x3, int y0, int y1, int y2, int y3) list {
   mut z0, z1, z2, z3 = 0, 0, 0, 0
   mut v0, v1, v2, v3 = y0, y1, y2, y3
   mut i = 0
   while(i < 128){
      def bit = i < 32 ? ((x0 >> (31 - i)) & 1) : (i < 64 ? ((x1 >> (63 - i)) & 1) : (i < 96 ? ((x2 >> (95 - i)) & 1) : ((x3 >> (127 - i)) & 1)))
      if(bit != 0){
         z0 = (z0 ^^ v0) & 0xffffffff
         z1 = (z1 ^^ v1) & 0xffffffff
         z2 = (z2 ^^ v2) & 0xffffffff
         z3 = (z3 ^^ v3) & 0xffffffff
      }
      def reduce = v3 & 1
      v3 = ((v3 >> 1) | ((v2 & 1) << 31)) & 0xffffffff
      v2 = ((v2 >> 1) | ((v1 & 1) << 31)) & 0xffffffff
      v1 = ((v1 >> 1) | ((v0 & 1) << 31)) & 0xffffffff
      v0 = (v0 >> 1) & 0x7fffffff
      if(reduce != 0){ v0 = (v0 ^^ 0xe1000000) & 0xffffffff }
      i += 1
   }
   [z0, z1, z2, z3]
}

fn gf128_mult(list x, list y) list {
   "Multiply two 128-bit values in GF(2^128) with GCM reduction polynomial.
   x, y: 16-byte lists(big-endian). Returns the 16-byte product."
   def xw = _gf128_words(x)
   def yw = _gf128_words(y)
   def zw = _gf128_mult_words(__load_item_fast(xw, 0), __load_item_fast(xw, 1), __load_item_fast(xw, 2), __load_item_fast(xw, 3), __load_item_fast(yw, 0), __load_item_fast(yw, 1), __load_item_fast(yw, 2), __load_item_fast(yw, 3))
   _gf128_words_to_bytes(__load_item_fast(zw, 0), __load_item_fast(zw, 1), __load_item_fast(zw, 2), __load_item_fast(zw, 3))
}

fn _gf128_xor(list a, list b) list {
   mut out = []
   mut i = 0
   while(i < 16){
      out = out.append(a.get(i) ^^ b.get(i))
      i += 1
   }
   out
}

fn _gf128_is_zero(list a) bool {
   mut i = 0
   while(i < 16){
      if(a.get(i) != 0){ return false }
      i += 1
   }
   true
}

fn gf128_inv(list x) any {
   "Multiplicative inverse in GF(2^128), or nil for zero."
   if(_gf128_is_zero(x)){ return nil }
   mut result = _gf128_one()
   mut i = 127
   while(i >= 0){
      result = gf128_mult(result, result)
      if(i > 0){ result = gf128_mult(result, x) }
      i -= 1
   }
   result
}

fn gf128_sqrt(list x) list {
   "Square root in GF(2^128): x^(2^127)."
   mut y, i = clone(x), 0
   while(i < 127){
      y = gf128_mult(y, y)
      i += 1
   }
   y
}

fn ghash(list h, list blocks) list {
   "Compute GHASH(H, blocks) for GCM authentication.
   h: 16-byte authentication key H = E_K(0^128).
   blocks: list of 16-byte data blocks(padded to 16 bytes each).
   Returns 16-byte GHASH output."
   def hw = _gf128_words(h)
   def h0, h1 = __load_item_fast(hw, 0), __load_item_fast(hw, 1)
   def h2, h3 = __load_item_fast(hw, 2), __load_item_fast(hw, 3)
   mut y0, y1, y2, y3 = 0, 0, 0, 0
   mut j = 0
   while(j < blocks.len){
      def block = __load_item_fast(blocks, j)
      y0 = (y0 ^^ _gcm_be32_at_padded(block, 0)) & 0xffffffff
      y1 = (y1 ^^ _gcm_be32_at_padded(block, 4)) & 0xffffffff
      y2 = (y2 ^^ _gcm_be32_at_padded(block, 8)) & 0xffffffff
      y3 = (y3 ^^ _gcm_be32_at_padded(block, 12)) & 0xffffffff
      def zw = _gf128_mult_words(y0, y1, y2, y3, h0, h1, h2, h3)
      y0 = __load_item_fast(zw, 0)
      y1 = __load_item_fast(zw, 1)
      y2 = __load_item_fast(zw, 2)
      y3 = __load_item_fast(zw, 3)
      j += 1
   }
   _gf128_words_to_bytes(y0, y1, y2, y3)
}

fn gcm_ghash(list h, list data) list {
   "Compute GHASH over already-padded 16-byte blocks with subkey h."
   def hw = _gf128_words(h)
   def h0, h1 = __load_item_fast(hw, 0), __load_item_fast(hw, 1)
   def h2, h3 = __load_item_fast(hw, 2), __load_item_fast(hw, 3)
   mut y0, y1, y2, y3 = 0, 0, 0, 0
   mut p = 0
   while(p < data.len){
      y0 = (y0 ^^ _gcm_be32_at_padded(data, p)) & 0xffffffff
      y1 = (y1 ^^ _gcm_be32_at_padded(data, p + 4)) & 0xffffffff
      y2 = (y2 ^^ _gcm_be32_at_padded(data, p + 8)) & 0xffffffff
      y3 = (y3 ^^ _gcm_be32_at_padded(data, p + 12)) & 0xffffffff
      def zw = _gf128_mult_words(y0, y1, y2, y3, h0, h1, h2, h3)
      y0 = __load_item_fast(zw, 0)
      y1 = __load_item_fast(zw, 1)
      y2 = __load_item_fast(zw, 2)
      y3 = __load_item_fast(zw, 3)
      p += 16
   }
   _gf128_words_to_bytes(y0, y1, y2, y3)
}

fn _gcm_ghash_update_data(int y0, int y1, int y2, int y3, int h0, int h1, int h2, int h3, list data) list {
   mut a0, a1, a2, a3 = y0, y1, y2, y3
   mut p = 0
   while(p < data.len){
      a0 = (a0 ^^ _gcm_be32_at_padded(data, p)) & 0xffffffff
      a1 = (a1 ^^ _gcm_be32_at_padded(data, p + 4)) & 0xffffffff
      a2 = (a2 ^^ _gcm_be32_at_padded(data, p + 8)) & 0xffffffff
      a3 = (a3 ^^ _gcm_be32_at_padded(data, p + 12)) & 0xffffffff
      def zw = _gf128_mult_words(a0, a1, a2, a3, h0, h1, h2, h3)
      a0 = __load_item_fast(zw, 0)
      a1 = __load_item_fast(zw, 1)
      a2 = __load_item_fast(zw, 2)
      a3 = __load_item_fast(zw, 3)
      p += 16
   }
   [a0, a1, a2, a3]
}

fn _gcm_u64_be_bits(any n) list {
   def bits = n * 8
   [
      (bits >> 56) & 255, (bits >> 48) & 255, (bits >> 40) & 255, (bits >> 32) & 255,
      (bits >> 24) & 255, (bits >> 16) & 255, (bits >> 8) & 255, bits & 255
   ]
}

fn _gcm_pad16(list data) list {
   mut out = clone(data)
   while(out.len % 16 != 0){ out = out.append(0) }
   out
}

fn _gcm_xor(list a, list b, any n=nil) list {
   def lim = n == nil ? (a.len < b.len ? a.len : b.len) : n
   mut out = []
   mut i = 0
   while(i < lim){
      out = out.append(a[i] ^^ b[i])
      i += 1
   }
   out
}

fn _gcm_join(list a, list b) list {
   mut out = clone(a)
   mut i = 0
   while(i < b.len){
      out = out.append(b[i])
      i += 1
   }
   out
}

fn _gcm_inc32(list block) list {
   mut out = clone(block)
   def ctr = (((out[12] << 24) | (out[13] << 16) | (out[14] << 8) | out[15]) + 1) & 0xffffffff
   out[12] = (ctr >> 24) & 255
   out[13] = (ctr >> 16) & 255
   out[14] = (ctr >> 8) & 255
   out[15] = ctr & 255
   out
}

fn _gcm_j0(list h, list nonce) list {
   if(nonce.len == 12){ return clone(nonce).append(0).append(0).append(0).append(1) }
   def len_block = _gcm_join([0,0,0,0,0,0,0,0], _gcm_u64_be_bits(nonce.len))
   gcm_ghash(h, _gcm_join(_gcm_pad16(nonce), len_block))
}

fn _gcm_ctr_crypt(any ctx, list j0, list data) list {
   mut ctr = _gcm_inc32(j0)
   mut out = []
   mut p = 0
   while(p < data.len){
      def stream = aes_encrypt_block(ctx, ctr)
      def block = slice(data, p, p + 16)
      out = _gcm_join(out, _gcm_xor(block, stream, block.len))
      ctr = _gcm_inc32(ctr)
      p += 16
   }
   out
}

fn gcm_auth_ghash(list h, list ad, list ciphertext) list {
   "Compute the GCM authentication GHASH over associated data and ciphertext."
   def hw = _gf128_words(h)
   def h0, h1 = __load_item_fast(hw, 0), __load_item_fast(hw, 1)
   def h2, h3 = __load_item_fast(hw, 2), __load_item_fast(hw, 3)
   def aw = _gcm_ghash_update_data(0, 0, 0, 0, h0, h1, h2, h3, ad)
   def cw = _gcm_ghash_update_data(__load_item_fast(aw, 0), __load_item_fast(aw, 1), __load_item_fast(aw, 2), __load_item_fast(aw, 3), h0, h1, h2, h3, ciphertext)
   def ad_bits = ad.len * 8
   def ct_bits = ciphertext.len * 8
   def y0 = (__load_item_fast(cw, 0) ^^ ((ad_bits >> 32) & 0xffffffff)) & 0xffffffff
   def y1 = (__load_item_fast(cw, 1) ^^ (ad_bits & 0xffffffff)) & 0xffffffff
   def y2 = (__load_item_fast(cw, 2) ^^ ((ct_bits >> 32) & 0xffffffff)) & 0xffffffff
   def y3 = (__load_item_fast(cw, 3) ^^ (ct_bits & 0xffffffff)) & 0xffffffff
   def zw = _gf128_mult_words(y0, y1, y2, y3, h0, h1, h2, h3)
   _gf128_words_to_bytes(__load_item_fast(zw, 0), __load_item_fast(zw, 1), __load_item_fast(zw, 2), __load_item_fast(zw, 3))
}

fn gcm_encrypt(list key, list nonce, list ad, list plaintext) list {
   "Authenticated Encryption using AES-GCM. Returns [ciphertext, tag]."
   def ctx = aes_init(key)
   def h = aes_encrypt_block(ctx, [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0])
   def j0 = _gcm_j0(h, nonce)
   def ciphertext = _gcm_ctr_crypt(ctx, j0, plaintext)
   def s = gcm_auth_ghash(h, ad, ciphertext)
   def e0 = aes_encrypt_block(ctx, j0)
   [ciphertext, _gcm_xor(s, e0, 16)]
}

fn gcm_verify_tag(list a, list b) bool {
   "Constant-shape tag comparison for GCM tags."
   if(a.len != b.len){ return false }
   mut diff = 0
   mut i = 0
   while(i < a.len){
      diff = diff | (a[i] ^^ b[i])
      i += 1
   }
   diff == 0
}

fn gcm_decrypt(list key, list nonce, list ad, list ciphertext, list tag) any {
   "Authenticated AES-GCM decryption. Returns plaintext, or nil when tag verification fails."
   def ctx = aes_init(key)
   def h = aes_encrypt_block(ctx, [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0])
   def j0 = _gcm_j0(h, nonce)
   def s = gcm_auth_ghash(h, ad, ciphertext)
   def e0 = aes_encrypt_block(ctx, j0)
   def expected = _gcm_xor(s, e0, 16)
   if(!gcm_verify_tag(expected, tag)){ return nil }
   _gcm_ctr_crypt(ctx, j0, ciphertext)
}

fn gcm_recover_keystream(list ct, list known_pt) list {
   "Recover GCM keystream bytes from ciphertext and known plaintext.
   ct: ciphertext bytes. known_pt: known plaintext bytes(any prefix).
   Returns keystream bytes."
   def n = known_pt.len
   mut ks = []
   mut i = 0
   while(i < n){
      ks = ks.append(ct.get(i) ^^ known_pt.get(i))
      i += 1
   }
   ks
}

fn gcm_nonce_reuse_decrypt(list ct1, list tag1, list ct2, list tag2) list {
   "Exploit GCM nonce reuse: two messages encrypted with same(K, IV).
   XOR of ciphertexts = XOR of plaintexts. Keystream can be recovered
   if any plaintext is known.
   Returns [xor_pt, ks_xor] where xor_pt = pt1 XOR pt2 at overlapping bytes,
   and ks_xor = ct1 XOR ct2."
   def n1, n2 = ct1.len, ct2.len
   def n = n1 < n2 ? n1 : n2
   mut ks_xor = []
   mut i = 0
   while(i < n){
      ks_xor = ks_xor.append(ct1.get(i) ^^ ct2.get(i))
      i += 1
   }
   [ks_xor, ks_xor]
}

fn gcm_forge_tag(list h, list assoc_data, list ct, list ectr0) list {
   "Forge a GCM authentication tag given knowledge of the authentication key H
   and the keystream block E(K, ctr0).
   h: 16-byte H value. assoc_data: associated data bytes(padded to 16).
   ct: ciphertext bytes. ectr0: E(K, ctr0) as 16 bytes.
   Returns the forged 16-byte tag."
   def g = gcm_auth_ghash(h, assoc_data, ct)
   mut tag = []
   mut i = 0
   while(i < 16){
      tag = tag.append(g.get(i) ^^ ectr0.get(i))
      i += 1
   }
   tag
}

fn gcm_recover_ectr0_from_sample(any h, list assoc_data, list ct, any tag) any {
   "Recover E(K,J0) from one valid GCM tuple under a known H.
   ectr0 = tag XOR GHASH(H, A, C)."
   if(!is_list(h) || h.len != 16){ return nil }
   if(!is_list(tag) || tag.len != 16){ return nil }
   def forged = gcm_forge_tag(h, assoc_data, ct, _gf128_zero())
   if(!is_list(forged) || forged.len != 16){ return nil }
   mut out = []
   mut i = 0
   while(i < 16){
      out = out.append(tag.get(i) ^^ forged.get(i))
      i += 1
   }
   out
}

fn gcm_forge_tag_from_known_message(list h, list known_assoc_data, list known_ct, list known_tag, list target_assoc_data, list target_ct) any {
   "Forge a tag for target(A,C) using nonce reuse and known H from one valid tuple.
   Derives E(K,J0) from the known tuple and reuses it on the target."
   def ectr0 = gcm_recover_ectr0_from_sample(h, known_assoc_data, known_ct, known_tag)
   if(ectr0 == nil){ return nil }
   gcm_forge_tag(h, target_assoc_data, target_ct, ectr0)
}

fn recover_e0(list h, list assoc_data, list ct, list tag) any {
   "Short export wrapper for recovering E(K,J0)."
   gcm_recover_ectr0_from_sample(h, assoc_data, ct, tag)
}

fn forge_tag_from_known(list h, list known_assoc_data, list known_ct, list known_tag, list target_assoc_data, list target_ct) any {
   "Short export wrapper for forging a tag from one known tuple."
   gcm_forge_tag_from_known_message(h, known_assoc_data, known_ct, known_tag, target_assoc_data, target_ct)
}

fn gcm_recover_auth_key_one_block(list a1, list c1, list t1, list a2, list c2, list t2) any {
   "Recover the GCM authentication subkey H for the common Joux nonce-reuse case:
   no AAD, one equal-length ciphertext block, same nonce/key. Returns H or nil.
   For one-block messages with equal lengths, tag1^tag2 = (c1^c2) * H^2."
   if(a1.len != 0 || a2.len != 0){ return nil }
   if(c1.len != 16 || c2.len != 16 || t1.len != 16 || t2.len != 16){ return nil }
   def dc = _gf128_xor(c1, c2)
   if(_gf128_is_zero(dc)){ return nil }
   def dt = _gf128_xor(t1, t2)
   def inv_dc = gf128_inv(dc)
   if(inv_dc == nil){ return nil }
   gf128_sqrt(gf128_mult(dt, inv_dc))
}
