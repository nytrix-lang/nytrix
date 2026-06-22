;; Keywords: symmetric aes math crypto
;; Symmetric-crypto routines for AES block encryption, key schedule, and analysis primitives.
;; Reference:
;; - https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.197-upd1.pdf
;; Implementation of AES-128, AES-192, and AES-256.
;; References:
;; - std.math.crypto.symmetric
;; - std.math.crypto
module std.math.crypto.symmetric.aes(aes_encrypt_block, aes_decrypt_block, aes_init, aes_encrypt_ecb, aes_decrypt_ecb, aes_encrypt_cbc, aes_decrypt_cbc, aes_encrypt_ctr, aes_sbox, aes_inv_sbox, aes_matrix_to_bytes, aes_add_round_key_matrix, aes_sub_bytes_matrix, aes_inv_shift_rows_matrix, aes_inv_mix_columns_matrix)
use std.core
use std.math.bin (unpack_be32)
use std.math.simmd as simmd

fn _build_aes_sbox() list {
   mut sbox = list()
   sbox = sbox.append(0x63) sbox = sbox.append(0x7c) sbox = sbox.append(0x77) sbox = sbox.append(0x7b) sbox = sbox.append(0xf2)
   sbox = sbox.append(0x6b) sbox = sbox.append(0x6f) sbox = sbox.append(0xc5) sbox = sbox.append(0x30) sbox = sbox.append(0x01)
   sbox = sbox.append(0x67) sbox = sbox.append(0x2b) sbox = sbox.append(0xfe) sbox = sbox.append(0xd7) sbox = sbox.append(0xab)
   sbox = sbox.append(0x76) sbox = sbox.append(0xca) sbox = sbox.append(0x82) sbox = sbox.append(0xc9) sbox = sbox.append(0x7d)
   sbox = sbox.append(0xfa) sbox = sbox.append(0x59) sbox = sbox.append(0x47) sbox = sbox.append(0xf0) sbox = sbox.append(0xad)
   sbox = sbox.append(0xd4) sbox = sbox.append(0xa2) sbox = sbox.append(0xaf) sbox = sbox.append(0x9c) sbox = sbox.append(0xa4)
   sbox = sbox.append(0x72) sbox = sbox.append(0xc0) sbox = sbox.append(0xb7) sbox = sbox.append(0xfd) sbox = sbox.append(0x93)
   sbox = sbox.append(0x26) sbox = sbox.append(0x36) sbox = sbox.append(0x3f) sbox = sbox.append(0xf7) sbox = sbox.append(0xcc)
   sbox = sbox.append(0x34) sbox = sbox.append(0xa5) sbox = sbox.append(0xe5) sbox = sbox.append(0xf1) sbox = sbox.append(0x71)
   sbox = sbox.append(0xd8) sbox = sbox.append(0x31) sbox = sbox.append(0x15) sbox = sbox.append(0x04) sbox = sbox.append(0xc7)
   sbox = sbox.append(0x23) sbox = sbox.append(0xc3) sbox = sbox.append(0x18) sbox = sbox.append(0x96) sbox = sbox.append(0x05)
   sbox = sbox.append(0x9a) sbox = sbox.append(0x07) sbox = sbox.append(0x12) sbox = sbox.append(0x80) sbox = sbox.append(0xe2)
   sbox = sbox.append(0xeb) sbox = sbox.append(0x27) sbox = sbox.append(0xb2) sbox = sbox.append(0x75) sbox = sbox.append(0x09)
   sbox = sbox.append(0x83) sbox = sbox.append(0x2c) sbox = sbox.append(0x1a) sbox = sbox.append(0x1b) sbox = sbox.append(0x6e)
   sbox = sbox.append(0x5a) sbox = sbox.append(0xa0) sbox = sbox.append(0x52) sbox = sbox.append(0x3b) sbox = sbox.append(0xd6)
   sbox = sbox.append(0xb3) sbox = sbox.append(0x29) sbox = sbox.append(0xe3) sbox = sbox.append(0x2f) sbox = sbox.append(0x84)
   sbox = sbox.append(0x53) sbox = sbox.append(0xd1) sbox = sbox.append(0x00) sbox = sbox.append(0xed) sbox = sbox.append(0x20)
   sbox = sbox.append(0xfc) sbox = sbox.append(0xb1) sbox = sbox.append(0x5b) sbox = sbox.append(0x6a) sbox = sbox.append(0xcb)
   sbox = sbox.append(0xbe) sbox = sbox.append(0x39) sbox = sbox.append(0x4a) sbox = sbox.append(0x4c) sbox = sbox.append(0x58)
   sbox = sbox.append(0xcf) sbox = sbox.append(0xd0) sbox = sbox.append(0xef) sbox = sbox.append(0xaa) sbox = sbox.append(0xfb)
   sbox = sbox.append(0x43) sbox = sbox.append(0x4d) sbox = sbox.append(0x33) sbox = sbox.append(0x85) sbox = sbox.append(0x45)
   sbox = sbox.append(0xf9) sbox = sbox.append(0x02) sbox = sbox.append(0x7f) sbox = sbox.append(0x50) sbox = sbox.append(0x3c)
   sbox = sbox.append(0x9f) sbox = sbox.append(0xa8) sbox = sbox.append(0x51) sbox = sbox.append(0xa3) sbox = sbox.append(0x40)
   sbox = sbox.append(0x8f) sbox = sbox.append(0x92) sbox = sbox.append(0x9d) sbox = sbox.append(0x38) sbox = sbox.append(0xf5)
   sbox = sbox.append(0xbc) sbox = sbox.append(0xb6) sbox = sbox.append(0xda) sbox = sbox.append(0x21) sbox = sbox.append(0x10)
   sbox = sbox.append(0xff) sbox = sbox.append(0xf3) sbox = sbox.append(0xd2) sbox = sbox.append(0xcd) sbox = sbox.append(0x0c)
   sbox = sbox.append(0x13) sbox = sbox.append(0xec) sbox = sbox.append(0x5f) sbox = sbox.append(0x97) sbox = sbox.append(0x44)
   sbox = sbox.append(0x17) sbox = sbox.append(0xc4) sbox = sbox.append(0xa7) sbox = sbox.append(0x7e) sbox = sbox.append(0x3d)
   sbox = sbox.append(0x64) sbox = sbox.append(0x5d) sbox = sbox.append(0x19) sbox = sbox.append(0x73) sbox = sbox.append(0x60)
   sbox = sbox.append(0x81) sbox = sbox.append(0x4f) sbox = sbox.append(0xdc) sbox = sbox.append(0x22) sbox = sbox.append(0x2a)
   sbox = sbox.append(0x90) sbox = sbox.append(0x88) sbox = sbox.append(0x46) sbox = sbox.append(0xee) sbox = sbox.append(0xb8)
   sbox = sbox.append(0x14) sbox = sbox.append(0xde) sbox = sbox.append(0x5e) sbox = sbox.append(0x0b) sbox = sbox.append(0xdb)
   sbox = sbox.append(0xe0) sbox = sbox.append(0x32) sbox = sbox.append(0x3a) sbox = sbox.append(0x0a) sbox = sbox.append(0x49)
   sbox = sbox.append(0x06) sbox = sbox.append(0x24) sbox = sbox.append(0x5c) sbox = sbox.append(0xc2) sbox = sbox.append(0xd3)
   sbox = sbox.append(0xac) sbox = sbox.append(0x62) sbox = sbox.append(0x91) sbox = sbox.append(0x95) sbox = sbox.append(0xe4)
   sbox = sbox.append(0x79) sbox = sbox.append(0xe7) sbox = sbox.append(0xc8) sbox = sbox.append(0x37) sbox = sbox.append(0x6d)
   sbox = sbox.append(0x8d) sbox = sbox.append(0xd5) sbox = sbox.append(0x4e) sbox = sbox.append(0xa9) sbox = sbox.append(0x6c)
   sbox = sbox.append(0x56) sbox = sbox.append(0xf4) sbox = sbox.append(0xea) sbox = sbox.append(0x65) sbox = sbox.append(0x7a)
   sbox = sbox.append(0xae) sbox = sbox.append(0x08) sbox = sbox.append(0xba) sbox = sbox.append(0x78) sbox = sbox.append(0x25)
   sbox = sbox.append(0x2e) sbox = sbox.append(0x1c) sbox = sbox.append(0xa6) sbox = sbox.append(0xb4) sbox = sbox.append(0xc6)
   sbox = sbox.append(0xe8) sbox = sbox.append(0xdd) sbox = sbox.append(0x74) sbox = sbox.append(0x1f) sbox = sbox.append(0x4b)
   sbox = sbox.append(0xbd) sbox = sbox.append(0x8b) sbox = sbox.append(0x8a) sbox = sbox.append(0x70) sbox = sbox.append(0x3e)
   sbox = sbox.append(0xb5) sbox = sbox.append(0x66) sbox = sbox.append(0x48) sbox = sbox.append(0x03) sbox = sbox.append(0xf6)
   sbox = sbox.append(0x0e) sbox = sbox.append(0x61) sbox = sbox.append(0x35) sbox = sbox.append(0x57) sbox = sbox.append(0xb9)
   sbox = sbox.append(0x86) sbox = sbox.append(0xc1) sbox = sbox.append(0x1d) sbox = sbox.append(0x9e) sbox = sbox.append(0xe1)
   sbox = sbox.append(0xf8) sbox = sbox.append(0x98) sbox = sbox.append(0x11) sbox = sbox.append(0x69) sbox = sbox.append(0xd9)
   sbox = sbox.append(0x8e) sbox = sbox.append(0x94) sbox = sbox.append(0x9b) sbox = sbox.append(0x1e) sbox = sbox.append(0x87)
   sbox = sbox.append(0xe9) sbox = sbox.append(0xce) sbox = sbox.append(0x55) sbox = sbox.append(0x28) sbox = sbox.append(0xdf)
   sbox = sbox.append(0x8c) sbox = sbox.append(0xa1) sbox = sbox.append(0x89) sbox = sbox.append(0x0d) sbox = sbox.append(0xbf)
   sbox = sbox.append(0xe6) sbox = sbox.append(0x42) sbox = sbox.append(0x68) sbox = sbox.append(0x41) sbox = sbox.append(0x99)
   sbox = sbox.append(0x2d) sbox = sbox.append(0x0f) sbox = sbox.append(0xb0) sbox = sbox.append(0x54) sbox = sbox.append(0xbb)
   sbox = sbox.append(0x16) sbox
}

def AES_SBOX = _build_aes_sbox()

fn _build_aes_inv_sbox() list {
   mut sbox = list()
   sbox = sbox.append(0x52) sbox = sbox.append(0x09) sbox = sbox.append(0x6a) sbox = sbox.append(0xd5) sbox = sbox.append(0x30)
   sbox = sbox.append(0x36) sbox = sbox.append(0xa5) sbox = sbox.append(0x38) sbox = sbox.append(0xbf) sbox = sbox.append(0x40)
   sbox = sbox.append(0xa3) sbox = sbox.append(0x9e) sbox = sbox.append(0x81) sbox = sbox.append(0xf3) sbox = sbox.append(0xd7)
   sbox = sbox.append(0xfb) sbox = sbox.append(0x7c) sbox = sbox.append(0xe3) sbox = sbox.append(0x39) sbox = sbox.append(0x82)
   sbox = sbox.append(0x9b) sbox = sbox.append(0x2f) sbox = sbox.append(0xff) sbox = sbox.append(0x87) sbox = sbox.append(0x34)
   sbox = sbox.append(0x8e) sbox = sbox.append(0x43) sbox = sbox.append(0x44) sbox = sbox.append(0xc4) sbox = sbox.append(0xde)
   sbox = sbox.append(0xe9) sbox = sbox.append(0xcb) sbox = sbox.append(0x54) sbox = sbox.append(0x7b) sbox = sbox.append(0x94)
   sbox = sbox.append(0x32) sbox = sbox.append(0xa6) sbox = sbox.append(0xc2) sbox = sbox.append(0x23) sbox = sbox.append(0x3d)
   sbox = sbox.append(0xee) sbox = sbox.append(0x4c) sbox = sbox.append(0x95) sbox = sbox.append(0x0b) sbox = sbox.append(0x42)
   sbox = sbox.append(0xfa) sbox = sbox.append(0xc3) sbox = sbox.append(0x4e) sbox = sbox.append(0x08) sbox = sbox.append(0x2e)
   sbox = sbox.append(0xa1) sbox = sbox.append(0x66) sbox = sbox.append(0x28) sbox = sbox.append(0xd9) sbox = sbox.append(0x24)
   sbox = sbox.append(0xb2) sbox = sbox.append(0x76) sbox = sbox.append(0x5b) sbox = sbox.append(0xa2) sbox = sbox.append(0x49)
   sbox = sbox.append(0x6d) sbox = sbox.append(0x8b) sbox = sbox.append(0xd1) sbox = sbox.append(0x25) sbox = sbox.append(0x72)
   sbox = sbox.append(0xf8) sbox = sbox.append(0xf6) sbox = sbox.append(0x64) sbox = sbox.append(0x86) sbox = sbox.append(0x68)
   sbox = sbox.append(0x98) sbox = sbox.append(0x16) sbox = sbox.append(0xd4) sbox = sbox.append(0xa4) sbox = sbox.append(0x5c)
   sbox = sbox.append(0xcc) sbox = sbox.append(0x5d) sbox = sbox.append(0x65) sbox = sbox.append(0xb6) sbox = sbox.append(0x92)
   sbox = sbox.append(0x6c) sbox = sbox.append(0x70) sbox = sbox.append(0x48) sbox = sbox.append(0x50) sbox = sbox.append(0xfd)
   sbox = sbox.append(0xed) sbox = sbox.append(0xb9) sbox = sbox.append(0xda) sbox = sbox.append(0x5e) sbox = sbox.append(0x15)
   sbox = sbox.append(0x46) sbox = sbox.append(0x57) sbox = sbox.append(0xa7) sbox = sbox.append(0x8d) sbox = sbox.append(0x9d)
   sbox = sbox.append(0x84) sbox = sbox.append(0x90) sbox = sbox.append(0xd8) sbox = sbox.append(0xab) sbox = sbox.append(0x00)
   sbox = sbox.append(0x8c) sbox = sbox.append(0xbc) sbox = sbox.append(0xd3) sbox = sbox.append(0x0a) sbox = sbox.append(0xf7)
   sbox = sbox.append(0xe4) sbox = sbox.append(0x58) sbox = sbox.append(0x05) sbox = sbox.append(0xb8) sbox = sbox.append(0xb3)
   sbox = sbox.append(0x45) sbox = sbox.append(0x06) sbox = sbox.append(0xd0) sbox = sbox.append(0x2c) sbox = sbox.append(0x1e)
   sbox = sbox.append(0x8f) sbox = sbox.append(0xca) sbox = sbox.append(0x3f) sbox = sbox.append(0x0f) sbox = sbox.append(0x02)
   sbox = sbox.append(0xc1) sbox = sbox.append(0xaf) sbox = sbox.append(0xbd) sbox = sbox.append(0x03) sbox = sbox.append(0x01)
   sbox = sbox.append(0x13) sbox = sbox.append(0x8a) sbox = sbox.append(0x6b) sbox = sbox.append(0x3a) sbox = sbox.append(0x91)
   sbox = sbox.append(0x11) sbox = sbox.append(0x41) sbox = sbox.append(0x4f) sbox = sbox.append(0x67) sbox = sbox.append(0xdc)
   sbox = sbox.append(0xea) sbox = sbox.append(0x97) sbox = sbox.append(0xf2) sbox = sbox.append(0xcf) sbox = sbox.append(0xce)
   sbox = sbox.append(0xf0) sbox = sbox.append(0xb4) sbox = sbox.append(0xe6) sbox = sbox.append(0x73) sbox = sbox.append(0x96)
   sbox = sbox.append(0xac) sbox = sbox.append(0x74) sbox = sbox.append(0x22) sbox = sbox.append(0xe7) sbox = sbox.append(0xad)
   sbox = sbox.append(0x35) sbox = sbox.append(0x85) sbox = sbox.append(0xe2) sbox = sbox.append(0xf9) sbox = sbox.append(0x37)
   sbox = sbox.append(0xe8) sbox = sbox.append(0x1c) sbox = sbox.append(0x75) sbox = sbox.append(0xdf) sbox = sbox.append(0x6e)
   sbox = sbox.append(0x47) sbox = sbox.append(0xf1) sbox = sbox.append(0x1a) sbox = sbox.append(0x71) sbox = sbox.append(0x1d)
   sbox = sbox.append(0x29) sbox = sbox.append(0xc5) sbox = sbox.append(0x89) sbox = sbox.append(0x6f) sbox = sbox.append(0xb7)
   sbox = sbox.append(0x62) sbox = sbox.append(0x0e) sbox = sbox.append(0xaa) sbox = sbox.append(0x18) sbox = sbox.append(0xbe)
   sbox = sbox.append(0x1b) sbox = sbox.append(0xfc) sbox = sbox.append(0x56) sbox = sbox.append(0x3e) sbox = sbox.append(0x4b)
   sbox = sbox.append(0xc6) sbox = sbox.append(0xd2) sbox = sbox.append(0x79) sbox = sbox.append(0x20) sbox = sbox.append(0x9a)
   sbox = sbox.append(0xdb) sbox = sbox.append(0xc0) sbox = sbox.append(0xfe) sbox = sbox.append(0x78) sbox = sbox.append(0xcd)
   sbox = sbox.append(0x5a) sbox = sbox.append(0xf4) sbox = sbox.append(0x1f) sbox = sbox.append(0xdd) sbox = sbox.append(0xa8)
   sbox = sbox.append(0x33) sbox = sbox.append(0x88) sbox = sbox.append(0x07) sbox = sbox.append(0xc7) sbox = sbox.append(0x31)
   sbox = sbox.append(0xb1) sbox = sbox.append(0x12) sbox = sbox.append(0x10) sbox = sbox.append(0x59) sbox = sbox.append(0x27)
   sbox = sbox.append(0x80) sbox = sbox.append(0xec) sbox = sbox.append(0x5f) sbox = sbox.append(0x60) sbox = sbox.append(0x51)
   sbox = sbox.append(0x7f) sbox = sbox.append(0xa9) sbox = sbox.append(0x19) sbox = sbox.append(0xb5) sbox = sbox.append(0x4a)
   sbox = sbox.append(0x0d) sbox = sbox.append(0x2d) sbox = sbox.append(0xe5) sbox = sbox.append(0x7a) sbox = sbox.append(0x9f)
   sbox = sbox.append(0x93) sbox = sbox.append(0xc9) sbox = sbox.append(0x9c) sbox = sbox.append(0xef) sbox = sbox.append(0xa0)
   sbox = sbox.append(0xe0) sbox = sbox.append(0x3b) sbox = sbox.append(0x4d) sbox = sbox.append(0xae) sbox = sbox.append(0x2a)
   sbox = sbox.append(0xf5) sbox = sbox.append(0xb0) sbox = sbox.append(0xc8) sbox = sbox.append(0xeb) sbox = sbox.append(0xbb)
   sbox = sbox.append(0x3c) sbox = sbox.append(0x83) sbox = sbox.append(0x53) sbox = sbox.append(0x99) sbox = sbox.append(0x61)
   sbox = sbox.append(0x17) sbox = sbox.append(0x2b) sbox = sbox.append(0x04) sbox = sbox.append(0x7e) sbox = sbox.append(0xba)
   sbox = sbox.append(0x77) sbox = sbox.append(0xd6) sbox = sbox.append(0x26) sbox = sbox.append(0xe1) sbox = sbox.append(0x69)
   sbox = sbox.append(0x14) sbox = sbox.append(0x63) sbox = sbox.append(0x55) sbox = sbox.append(0x21) sbox = sbox.append(0x0c)
   sbox = sbox.append(0x7d) sbox
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
   mut rcon = list()
   rcon = rcon.append(0x00) rcon = rcon.append(0x01) rcon = rcon.append(0x02) rcon = rcon.append(0x04) rcon = rcon.append(0x08)
   rcon = rcon.append(0x10) rcon = rcon.append(0x20) rcon = rcon.append(0x40) rcon = rcon.append(0x80) rcon = rcon.append(0x1b) rcon = rcon.append(0x36) rcon
}

def AES_RCON = _build_aes_rcon()

fn _xtime(int x) int {
   def y = (x << 1) & 0xff
   (x & 0x80) != 0 ? (y ^^ 0x1b) : y
}

fn _sub_word(any w) any {
   def b0 = AES_SBOX[w & 0xff]
   def b1 = AES_SBOX[(w >> 8) & 0xff] << 8
   def b2 = AES_SBOX[(w >> 16) & 0xff] << 16
   def b3 = AES_SBOX[(w >> 24) & 0xff] << 24
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
      if i % nk == 0 { temp = _sub_word(_rot_word(temp)) ^^ (AES_RCON[i / nk] << 24) } elif nk > 6 && i % nk == 4 { temp = _sub_word(temp) }
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
      def t = b0 ^^ b1 ^^ b2 ^^ b3
      def u = b0
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
      def u = _xtime(_xtime(b0 ^^ b2))
      def v = _xtime(_xtime(b1 ^^ b3))
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
