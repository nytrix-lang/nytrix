;; Keywords: block-cipher attack cnv
;; Block-cipher attack routines for chosen-nonce CBC/MD5 attack.
;; This variant uses AES blocks with an MD5-derived chaining mask:
;; the first mask is the IV, then each next mask is MD5(cipher_block).
module std.math.crypto.block.attack.cnv(cnv_md5_cbc_encrypt, cnv_md5_cbc_decrypt, cnv_cookie_iv_from_name)
use std.core
use std.math.bin (pkcs7_pad, pkcs7_unpad)
use std.math.crypto.hash as hash
use std.math.crypto.symmetric.aes

fn _cnv_xor_block(list: a, list: b): list {
   mut out = []
   mut i = 0
   while(i < 16){
      out = out.append(a[i] ^^ b[i])
      i += 1
   }
   out
}

fn _cnv_md5_bytes(list: block): list { hash.md5(block).unhex }

fn cnv_md5_cbc_encrypt(list: key, list: iv, list: plaintext): list {
   "Encrypt with the AES-CNV MD5-chained CBC variant.
   Returns iv || ciphertext. Plaintext is PKCS#7 padded."
   def ctx = aes_init(key)
   def padded = pkcs7_pad(plaintext, 16)
   mut h = clone(iv)
   mut out = clone(iv)
   mut p = 0
   while(p < padded.len){
      def block = slice(padded, p, p + 16)
      def ct = aes_encrypt_block(ctx, _cnv_xor_block(block, h))
      mut i = 0
      while(i < 16){
         out = out.append(ct[i])
         i += 1
      }
      h = _cnv_md5_bytes(ct)
      p += 16
   }
   out
}

fn cnv_md5_cbc_decrypt(list: key, list: ciphertext): ?list {
   "Decrypt iv || ciphertext from the AES-CNV MD5-chained CBC variant."
   if(ciphertext.len < 32 || ciphertext.len % 16 != 0){ return nil }
   def ctx = aes_init(key)
   def iv = slice(ciphertext, 0, 16)
   mut h = clone(iv)
   mut out = []
   mut p = 16
   while(p < ciphertext.len){
      def block = slice(ciphertext, p, p + 16)
      def dec = aes_decrypt_block(ctx, block)
      def pt = _cnv_xor_block(dec, h)
      mut i = 0
      while(i < 16){
         out = out.append(pt[i])
         i += 1
      }
      h = _cnv_md5_bytes(block)
      p += 16
   }
   pkcs7_unpad(out)
}

fn cnv_cookie_iv_from_name(list: name_bytes, list: hidden_md5_bytes): list {
   "Build a CNV registration IV as PKCS#7(name) XOR MD5(hidden)."
   def padded = pkcs7_pad(name_bytes, 16)
   _cnv_xor_block(slice(padded, 0, 16), hidden_md5_bytes)
}
