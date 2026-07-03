;; Keywords: block-cipher stream rc4 math crypto
;; Stream-cipher routines for RC4 KSA/PRGA and known-key decryption.
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc6229
;; References:
;; - std.math.crypto.block.stream
;; - std.math.crypto
module std.math.crypto.block.stream.rc4(rc4_ksa, rc4_prga, rc4_decrypt_known_key)
use std.core

fn rc4_ksa(list key) list {
   "RC4 Key Scheduling Algorithm(KSA).
   key: byte list of the secret key
   Returns the initialized permutation state as a byte list of length 256."
   mut state = list(256)
   store64(state, 256, 0)
   mut i = 0
   while i < 256 {
      __store_item_fast(state, i, i)
      i += 1
   }
   def key_len = key.len
   mut j = 0
   i = 0
   while i < 256 {
      def si = __load_item_fast(state, i)
      j = (j + si + __load_item_fast(key, i % key_len)) % 256
      __store_item_fast(state, i, __load_item_fast(state, j))
      __store_item_fast(state, j, si)
      i += 1
   }
   state
}

fn rc4_prga(list state, int n) list {
   "RC4 Pseudo-Random Generation Algorithm(PRGA).
   state: permutation state from KSA(will be cloned, not modified)
   n: number of keystream bytes to generate
   Returns the keystream as a byte list of length n."
   mut s, i = clone(state), 0
   mut j = 0
   mut keystream = list(n)
   store64(keystream, n, 0)
   mut k = 0
   while k < n {
      i = (i + 1) % 256
      def si = __load_item_fast(s, i)
      j = (j + si) % 256
      def sj = __load_item_fast(s, j)
      __store_item_fast(s, i, sj)
      __store_item_fast(s, j, si)
      def idx = (si + sj) % 256
      __store_item_fast(keystream, k, __load_item_fast(s, idx))
      k += 1
   }
   keystream
}

fn rc4_decrypt_known_key(list ct, list key) list {
   "Decrypt RC4 ciphertext when the key is known.
   ct: ciphertext byte list
   key: key byte list
   Returns the decrypted plaintext as a byte list."
   mut s = rc4_ksa(key)
   def ct_len = ct.len
   mut plaintext = list(ct_len)
   store64(plaintext, ct_len, 0)
   mut i, j, k = 0, 0, 0
   while k < ct_len {
      i = (i + 1) % 256
      def si = __load_item_fast(s, i)
      j = (j + si) % 256
      def sj = __load_item_fast(s, j)
      __store_item_fast(s, i, sj)
      __store_item_fast(s, j, si)
      def idx = (si + sj) % 256
      __store_item_fast(plaintext, k, __load_item_fast(ct, k) ^^ __load_item_fast(s, idx))
      k += 1
   }
   plaintext
}
