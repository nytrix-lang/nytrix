;; Keywords: symmetric aes math crypto
;; Symmetric-crypto routines for AES block encryption, key schedule, and analysis primitives.
;; Reference:
;; - https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.197-upd1.pdf
;; Implementation of AES-128, AES-192, and AES-256.
;; References:
;; - std.math.crypto.symmetric
;; - std.math.crypto
module std.math.crypto.symmetric.aes(aes_encrypt_block, aes_decrypt_block, aes_init,
   aes_encrypt_ecb, aes_decrypt_ecb, aes_encrypt_cbc, aes_decrypt_cbc, aes_encrypt_ctr,
   aes_sbox, aes_inv_sbox, aes_matrix_to_bytes, aes_add_round_key_matrix,
   aes_sub_bytes_matrix, aes_inv_shift_rows_matrix, aes_inv_mix_columns_matrix)
use std.core
use std.math.bin (unpack_be32)
use std.math.simmd as simmd

fn _build_aes_sbox() list {
   [
      0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
      0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
      0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
      0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
      0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
      0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
      0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
      0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
      0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
      0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
      0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
      0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
      0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
      0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
      0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
      0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
   ]
}

def AES_SBOX = _build_aes_sbox()

fn _build_aes_inv_sbox() list {
   mut inv = list(256)
   __list_set_len(inv, 256)
   mut i = 0
   while i < 256 {
      __store_item_fast(inv, __load_item_fast(AES_SBOX, i), i)
      i += 1
   }
   inv
}

def AES_INV_SBOX = _build_aes_inv_sbox()

fn aes_sbox() list {
   "Return the AES S-box as a 256-element list of bytes."
   AES_SBOX
}

fn aes_inv_sbox() list {
   "Return the AES inverse S-box as a 256-element list of bytes."
   AES_INV_SBOX
}

fn aes_matrix_to_bytes(list matrix) list {
   "Flatten a CryptoHack-style AES state matrix into bytes."
   mut out = []
   mut i = 0
   while i < matrix.len {
      mut j = 0
      while j < matrix[i].len {
         out = out.append(matrix[i][j])
         j += 1
      }
      i += 1
   }
   out
}

fn _aes_bytes_to_matrix(list bytes) list {
   mut out = []
   mut i = 0
   while i < bytes.len {
      out = out.append(slice(bytes, i, i + 4))
      i += 4
   }
   out
}

fn aes_add_round_key_matrix(list state, list round_key) list {
   "XOR two CryptoHack-style AES state matrices."
   mut out = []
   mut i = 0
   while i < state.len {
      mut row = []
      mut j = 0
      while j < state[i].len {
         row = row.append(state[i][j] ^^ round_key[i][j])
         j += 1
      }
      out = out.append(row)
      i += 1
   }
   out
}

fn aes_sub_bytes_matrix(list state, list sbox) list {
   "Apply an S-box to every byte in a CryptoHack-style AES state matrix."
   mut out = []
   mut i = 0
   while i < state.len {
      mut row = []
      mut j = 0
      while j < state[i].len {
         row = row.append(sbox[state[i][j]])
         j += 1
      }
      out = out.append(row)
      i += 1
   }
   out
}

fn aes_inv_shift_rows_matrix(list state) list {
   "Apply inverse ShiftRows to a CryptoHack-style AES state matrix."
   mut flat = aes_matrix_to_bytes(state)
   _inv_shift_rows(flat)
   _aes_bytes_to_matrix(flat)
}

fn aes_inv_mix_columns_matrix(list state) list {
   "Apply inverse MixColumns to a CryptoHack-style AES state matrix."
   mut flat = aes_matrix_to_bytes(state)
   _inv_mix_columns(flat)
   _aes_bytes_to_matrix(flat)
}

fn _build_aes_rcon() list {
   [0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36]
}

def AES_RCON = _build_aes_rcon()

fn _xtime(int x) int {
   def y = (x << 1) & 0xff
   (x & 0x80) != 0 ? (y ^^ 0x1b) : y
}

fn _sub_word(any w) any {
   def b0, b1 = AES_SBOX[w & 0xff], AES_SBOX[(w >> 8) & 0xff] << 8
   def b2, b3 = AES_SBOX[(w >> 16) & 0xff] << 16, AES_SBOX[(w >> 24) & 0xff] << 24
   b0 | b1 | b2 | b3
}

fn _rot_word(any w) any { simmd.rotl32(w, 8) }

fn aes_init(list key) list {
   "Key expansion for AES-128/192/256."
   def n = key.len
   def nk = n / 4
   def nr = (nk == 4) ? 10 : ((nk == 6) ? 12 : 14)
   def words = (nr + 1) * 4
   mut w = list(words)
   store64(w, words, 0)
   mut i = 0 while i < nk {
      w[i] = unpack_be32(key, i * 4)
      i += 1
   }
   while i < words {
      mut temp = w[i - 1]
      if i % nk == 0 {
         temp = _sub_word(_rot_word(temp)) ^^ (AES_RCON[i / nk] << 24)
      } elif nk > 6 && i % nk == 4 {
         temp = _sub_word(temp)
      }
      w[i] = w[i - nk] ^^ temp
      i += 1
   }
   [w, nr]
}

fn _sub_bytes(list st) any {
   mut i = 0
   while i < 16 {
      st[i] = AES_SBOX[st[i]]
      i += 1
   }
}

fn _shift_rows(list st) any {
   def t1 = st[1] st[1] = st[5] st[5] = st[9] st[9] = st[13] st[13] = t1
   def t2 = st[2] st[2] = st[10] st[10] = t2
   def t2_ = st[6] st[6] = st[14] st[14] = t2_
   def t3 = st[15] st[15] = st[11] st[11] = st[7] st[7] = st[3] st[3] = t3
}

fn _mix_columns(list st) any {
   mut i = 0 while i < 4 {
      def b0 = st[i*4] def b1 = st[i*4+1] def b2 = st[i*4+2] def b3 = st[i*4+3]
      def t, u = b0 ^^ b1 ^^ b2 ^^ b3, b0
      st[i*4] = b0 ^^ t ^^ _xtime(b0 ^^ b1)
      st[i*4+1] = b1 ^^ t ^^ _xtime(b1 ^^ b2)
      st[i*4+2] = b2 ^^ t ^^ _xtime(b2 ^^ b3)
      st[i*4+3] = b3 ^^ t ^^ _xtime(b3 ^^ u)
      i += 1
   }
}

fn _inv_sub_bytes(list st) any {
   mut i = 0
   while i < 16 {
      st[i] = AES_INV_SBOX[st[i]]
      i += 1
   }
}

fn _inv_shift_rows(list st) any {
   def t1 = st[13] st[13] = st[9] st[9] = st[5] st[5] = st[1] st[1] = t1
   def t2 = st[2] st[2] = st[10] st[10] = t2
   def t2_ = st[6] st[6] = st[14] st[14] = t2_
   def t3 = st[3] st[3] = st[7] st[7] = st[11] st[11] = st[15] st[15] = t3
}

fn _inv_mix_columns(list st) any {
   mut i = 0 while i < 4 {
      def b0 = st[i*4] def b1 = st[i*4+1] def b2 = st[i*4+2] def b3 = st[i*4+3]
      def u, v = _xtime(_xtime(b0 ^^ b2)), _xtime(_xtime(b1 ^^ b3))
      st[i*4] = b0 ^^ u
      st[i*4+1] = b1 ^^ v
      st[i*4+2] = b2 ^^ u
      st[i*4+3] = b3 ^^ v
      i += 1
   }
   _mix_columns(st)
}

fn _add_round_key(list st, list w, int r) any {
   mut i = 0 while i < 4 {
      def word = w[r*4 + i]
      st[i*4] = st[i*4] ^^ ((word >> 24) & 0xff)
      st[i*4+1] = st[i*4+1] ^^ ((word >> 16) & 0xff)
      st[i*4+2] = st[i*4+2] ^^ ((word >> 8)  & 0xff)
      st[i*4+3] = st[i*4+3] ^^ (word & 0xff)
      i += 1
   }
}

fn _aes_encrypt_state(list ctx, list st) list {
   def w = ctx[0]
   def nr = ctx[1]
   _add_round_key(st, w, 0)
   mut r = 1 while r < nr {
      _sub_bytes(st)
      _shift_rows(st)
      _mix_columns(st)
      _add_round_key(st, w, r)
      r += 1
   }
   _sub_bytes(st)
   _shift_rows(st)
   _add_round_key(st, w, nr)
   st
}

fn aes_encrypt_block(list ctx, list block) list {
   "Encrypt one 16-byte AES block with an initialized context."
   _aes_encrypt_state(ctx, clone(block))
}

fn _aes_decrypt_state(list ctx, list st) list {
   def w = ctx[0]
   def nr = ctx[1]
   _add_round_key(st, w, nr)
   mut r = nr - 1
   while r > 0 {
      _inv_shift_rows(st)
      _inv_sub_bytes(st)
      _add_round_key(st, w, r)
      _inv_mix_columns(st)
      r -= 1
   }
   _inv_shift_rows(st)
   _inv_sub_bytes(st)
   _add_round_key(st, w, 0)
   st
}

fn aes_decrypt_block(list ctx, list block) list {
   "Decrypt one 16-byte AES block with an initialized context."
   _aes_decrypt_state(ctx, clone(block))
}

fn aes_encrypt_ecb(list key, list plaintext) any {
   "Encrypt full AES blocks in ECB mode without padding. Returns nil on a partial block."
   if plaintext.len % 16 != 0 { return nil }
   def ctx = aes_init(key)
   mut out = []
   mut block = [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
   mut p = 0
   while p < plaintext.len {
      mut i = 0
      while i < 16 {
         block[i] = plaintext[p + i]
         i += 1
      }
      _aes_encrypt_state(ctx, block)
      i = 0
      while i < 16 {
         out = out.append(block[i])
         i += 1
      }
      p += 16
   }
   out
}

fn aes_decrypt_ecb(list key, list ciphertext) any {
   "Decrypt full AES blocks in ECB mode without unpadding. Returns nil on a partial block."
   if ciphertext.len % 16 != 0 { return nil }
   def ctx = aes_init(key)
   mut out = []
   mut block = [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
   mut p = 0
   while p < ciphertext.len {
      mut i = 0
      while i < 16 {
         block[i] = ciphertext[p + i]
         i += 1
      }
      _aes_decrypt_state(ctx, block)
      i = 0
      while i < 16 {
         out = out.append(block[i])
         i += 1
      }
      p += 16
   }
   out
}

fn aes_encrypt_cbc(list key, list iv, list plaintext) list {
   "Encrypt plaintext with AES-CBC and zero padding."
   def ctx = aes_init(key)
   mut prev = clone(iv)
   mut res = []
   mut block = [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
   mut p = 0 while p < plaintext.len {
      mut i = 0
      while i < 16 {
         def src = p + i
         block[i] = (src < plaintext.len ? plaintext[src] : 0) ^^ prev[i]
         i += 1
      }
      _aes_encrypt_state(ctx, block)
      mut j = 0
      while j < 16 {
         res = res.append(block[j])
         prev[j] = block[j]
         j += 1
      }
      p += 16
   }
   res
}

fn aes_decrypt_cbc(list key, list iv, list ciphertext) any {
   "Decrypt AES-CBC ciphertext blocks with the given key and IV."
   if ciphertext.len % 16 != 0 { return nil }
   def ctx = aes_init(key)
   mut prev = clone(iv)
   mut next_prev = [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
   mut block = [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
   mut res = []
   mut p = 0
   mut pos = 0
   while p < ciphertext.len {
      mut i = 0
      while i < 16 {
         def b = ciphertext[p + i]
         block[i] = b
         next_prev[i] = b
         i += 1
      }
      _aes_decrypt_state(ctx, block)
      i = 0
      while i < 16 {
         res = res.append(block[i] ^^ prev[i])
         i += 1
      }
      def tmp = prev
      prev = next_prev
      next_prev = tmp
      p += 16
   }
   res
}

fn _aes_inc128_inplace(list block) any {
   mut i = 15
   mut carry = 1
   while i >= 0 && carry != 0 {
      def v = block[i] + carry
      block[i] = v & 255
      carry = v >> 8
      i -= 1
   }
}

fn aes_encrypt_ctr(list key, list nonce_counter, list data) list {
   "AES-CTR encryption/decryption with a 16-byte initial counter block."
   def ctx = aes_init(key)
   mut ctr = clone(nonce_counter)
   mut out = []
   mut block = [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]
   mut p = 0
   while p < data.len {
      mut j = 0
      while j < 16 {
         block[j] = ctr[j]
         j += 1
      }
      _aes_encrypt_state(ctx, block)
      mut i = 0
      while i < 16 && p + i < data.len {
         out = out.append(data[p + i] ^^ block[i])
         i += 1
      }
      _aes_inc128_inplace(ctr)
      p += 16
   }
   out
}
