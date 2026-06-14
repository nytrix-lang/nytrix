;; Keywords: hash length-extension math crypto
;; Hash-analysis routines for MD/SHA length-extension padding and forgery.
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc1321
;; - https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.180-4.pdf
;; References:
;; - std.math.crypto.hash
;; - std.math.crypto
module std.math.crypto.hash.length_extension(md5_padding, sha512_padding, md5_length_extend, sha1_length_extend, sha256_length_extend, sha512_length_extend)
use std.core
use std.math.bin
use std.math.big (bigint_from_str, bigint_to_int)

def _MD5_K = [3614090360, 3905402710, 606105819, 3250441966, 4118548399, 1200080426, 2821735955, 4249261313, 1770035416, 2336552879, 4294925233, 2304563134, 1804603682, 4254626195, 2792965006, 1236535329, 4129170786, 3225465664, 643717713, 3921069994, 3593408605, 38016083, 3634488961, 3889429448, 568446438, 3275163606, 4107603335, 1163531501, 2850285829, 4243563512, 1735328473, 2368359562, 4294588738, 2272392833, 1839030562, 4259657740, 2763975236, 1272893353, 4139469664, 3200236656, 681279174, 3936430074, 3572445317, 76029189, 3654602809, 3873151461, 530742520, 3299628645, 4096336452, 1126891415, 2878612391, 4237533241, 1700485571, 2399980690, 4293915773, 2240044497, 1873313359, 4264355552, 2734768916, 1309151649, 4149444226, 3174756917, 718787259, 3951481745]
def _MD5_S = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21]
def _SHA256_K = [0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]

fn md5_padding(int msg_len_bits) list {
   "Compute MD5 padding bytes for a message of given bit length.
   MD5 padding: append 0x80, then zeros, then 64-bit little-endian length.
   Total padded message is congruent to 448 mod 512 bits before length field.
   msg_len_bits: original message length in bits
   Returns the padding bytes as a list."
   def msg_len_bytes = msg_len_bits / 8
   mut pad_len = 64 - (msg_len_bytes % 64)
   if pad_len <= 8 { pad_len = pad_len + 64 }
   mut padding = list(pad_len)
   store64(padding, pad_len, 0)
   __store_item_fast(padding, 0, 128)
   mut j = 0
   while j < 8 {
      __store_item_fast(padding, pad_len - 8 + j, (msg_len_bits >> (j * 8)) & 255)
      j += 1
   }
   padding
}

fn sha512_padding(int msg_len_bits) list {
   "Compute SHA-512 padding bytes for a message of given bit length."
   _sha_padding(msg_len_bits / 8, 128, 16)
}

fn md5_length_extend(str orig_hash_hex, int orig_len_bytes, list suffix_bytes) str {
   "Perform MD5 length extension attack.
   Given a valid MD5 hash of an unknown message(whose length we know),
   compute a valid MD5 hash of(original_message || padding || suffix).
   orig_hash_hex: original MD5 hash as hex string
   orig_len_bytes: original message length in bytes
   suffix_bytes: bytes to append
   Returns the extended MD5 hash as a hex string."
   def suffix_len = suffix_bytes.len
   def glue = md5_padding(orig_len_bytes * 8)
   def final_pad = md5_padding((orig_len_bytes + glue.len + suffix_len) * 8)
   def hash_bytes = orig_hash_hex.unhex
   def a = _u32_from_bytes_le(hash_bytes, 0)
   def b = _u32_from_bytes_le(hash_bytes, 4)
   def c = _u32_from_bytes_le(hash_bytes, 8)
   def d = _u32_from_bytes_le(hash_bytes, 12)
   def padded_suffix_len = suffix_len + final_pad.len
   mut M = list(padded_suffix_len)
   store64(M, padded_suffix_len, 0)
   mut si = 0
   while si < suffix_len {
      __store_item_fast(M, si, __load_item_fast(suffix_bytes, si))
      si += 1
   }
   mut pi = 0
   while pi < final_pad.len {
      __store_item_fast(M, suffix_len + pi, __load_item_fast(final_pad, pi))
      pi += 1
   }
   mut ha, hb = a, b
   mut hc, hd = c, d
   mut off = 0
   def num_blocks = padded_suffix_len / 64
   mut bi = 0
   while bi < num_blocks {
      mut aa = ha
      mut bb = hb
      mut cc = hc
      mut dd = hd
      mut ri = 0
      while ri < 64 {
         mut f, g = 0, 0
         if ri < 16 {
            f, g = ((bb & cc) | ((bb ^^ 4294967295) & dd)), ri
         } else if ri < 32 {
            f, g = ((dd & bb) | ((dd ^^ 4294967295) & cc)), (5 * ri + 1) % 16
         } else if ri < 48 {
            f, g = (bb ^^ cc ^^ dd), (3 * ri + 5) % 16
         } else {
            f, g = (cc ^^ (bb | (dd ^^ 4294967295))), (7 * ri) % 16
         }
         def gi = off + g * 4
         def M_g = __load_item_fast(M, gi) | (__load_item_fast(M, gi + 1) << 8) | (__load_item_fast(M, gi + 2) << 16) | (__load_item_fast(M, gi + 3) << 24)
         def temp = _u32(bb + _rotl32(_u32(aa + f + __load_item_fast(_MD5_K, ri) + M_g), __load_item_fast(_MD5_S, ri)))
         aa = dd
         dd = cc
         cc = bb
         bb = temp
         ri += 1
      }
      ha, hb = _u32(ha + aa), _u32(hb + bb)
      hc, hd = _u32(hc + cc), _u32(hd + dd)
      off = off + 64
      bi += 1
   }
   _u32_to_hex_le(ha) + _u32_to_hex_le(hb) + _u32_to_hex_le(hc) + _u32_to_hex_le(hd)
}

fn sha1_length_extend(str orig_hash_hex, int orig_len_bytes, list suffix_bytes) str {
   "Perform SHA-1 length extension attack.
   Given a valid SHA-1 hash of an unknown message,
   compute a valid SHA-1 hash of(original_message || padding || suffix).
   orig_hash_hex: original SHA-1 hash as hex string(40 chars)
   orig_len_bytes: original message length in bytes
   suffix_bytes: bytes to append
   Returns the extended SHA-1 hash as a hex string."
   def suffix_len = suffix_bytes.len
   def glue = _sha_padding(orig_len_bytes, 64, 8)
   def final_pad = _sha_padding(orig_len_bytes + glue.len + suffix_len, 64, 8)
   def hash_bytes = orig_hash_hex.unhex
   def h0 = _u32_from_bytes_be(hash_bytes, 0)
   def h1 = _u32_from_bytes_be(hash_bytes, 4)
   def h2 = _u32_from_bytes_be(hash_bytes, 8)
   def h3 = _u32_from_bytes_be(hash_bytes, 12)
   def h4 = _u32_from_bytes_be(hash_bytes, 16)
   def padded_suffix_len = suffix_len + final_pad.len
   mut M = list(padded_suffix_len)
   store64(M, padded_suffix_len, 0)
   mut si2 = 0
   while si2 < suffix_len {
      __store_item_fast(M, si2, __load_item_fast(suffix_bytes, si2))
      si2 += 1
   }
   mut pi2 = 0
   while pi2 < final_pad.len {
      __store_item_fast(M, suffix_len + pi2, __load_item_fast(final_pad, pi2))
      pi2 += 1
   }
   mut ha2, hb2 = h0, h1
   mut hc2, hd2 = h2, h3
   mut he2 = h4
   def num_blocks2 = padded_suffix_len / 64
   mut bi2 = 0
   mut W = list(80)
   store64(W, 80, 0)
   while bi2 < num_blocks2 {
      mut wi = 0
      while wi < 16 {
         def base = bi2 * 64 + wi * 4
         def word = (__load_item_fast(M, base) << 24) | (__load_item_fast(M, base + 1) << 16) | (__load_item_fast(M, base + 2) << 8) | __load_item_fast(M, base + 3)
         __store_item_fast(W, wi, _u32(word))
         wi += 1
      }
      while wi < 80 {
         def v = __load_item_fast(W, wi - 3) ^^ __load_item_fast(W, wi - 8) ^^ __load_item_fast(W, wi - 14) ^^ __load_item_fast(W, wi - 16)
         __store_item_fast(W, wi, _rotl32(v, 1))
         wi += 1
      }
      mut aa2 = ha2
      mut bb2 = hb2
      mut cc2 = hc2
      mut dd2 = hd2
      mut ee2 = he2
      mut ri2 = 0
      while ri2 < 80 {
         def ki = ri2 < 20 ? 1518500249 : (ri2 < 40 ? 1859775393 : (ri2 < 60 ? 2400959708 : 3395469782))
         mut f2 = 0
         if ri2 < 20 { f2 = ((bb2 & cc2) | ((bb2 ^^ 4294967295) & dd2)) } else if ri2 < 40 {
            f2 = (bb2 ^^ cc2 ^^ dd2)
         } else if ri2 < 60 {
            f2 = ((bb2 & cc2) | (bb2 & dd2) | (cc2 & dd2))
         } else {
            f2 = (bb2 ^^ cc2 ^^ dd2)
         }
         def temp2 = _u32(_rotl32(aa2, 5) + f2 + ee2 + ki + __load_item_fast(W, ri2))
         ee2 = dd2
         dd2 = cc2
         cc2 = _rotl32(bb2, 30)
         bb2 = aa2
         aa2 = temp2
         ri2 += 1
      }
      ha2, hb2 = _u32(ha2 + aa2), _u32(hb2 + bb2)
      hc2, hd2 = _u32(hc2 + cc2), _u32(hd2 + dd2)
      he2 = _u32(he2 + ee2)
      bi2 += 1
   }
   _u32_to_hex_be(ha2) + _u32_to_hex_be(hb2) + _u32_to_hex_be(hc2) + _u32_to_hex_be(hd2) + _u32_to_hex_be(he2)
}

fn sha256_length_extend(str orig_hash_hex, int orig_len_bytes, list suffix_bytes) str {
   "Perform SHA-256 length extension attack.
   Given a valid SHA-256 hash of an unknown message,
   compute a valid SHA-256 hash of(original_message || padding || suffix).
   orig_hash_hex: original SHA-256 hash as hex string(64 chars)
   orig_len_bytes: original message length in bytes
   suffix_bytes: bytes to append
   Returns the extended SHA-256 hash as a hex string."
   def suffix_len2 = suffix_bytes.len
   def glue = _sha_padding(orig_len_bytes, 64, 8)
   def final_pad = _sha_padding(orig_len_bytes + glue.len + suffix_len2, 64, 8)
   def hash_bytes = orig_hash_hex.unhex
   def h0 = _u32_from_bytes_be(hash_bytes, 0)
   def h1 = _u32_from_bytes_be(hash_bytes, 4)
   def h2 = _u32_from_bytes_be(hash_bytes, 8)
   def h3 = _u32_from_bytes_be(hash_bytes, 12)
   def h4 = _u32_from_bytes_be(hash_bytes, 16)
   def h5 = _u32_from_bytes_be(hash_bytes, 20)
   def h6 = _u32_from_bytes_be(hash_bytes, 24)
   def h7 = _u32_from_bytes_be(hash_bytes, 28)
   def padded_suffix_len2 = suffix_len2 + final_pad.len
   mut M2 = list(padded_suffix_len2)
   store64(M2, padded_suffix_len2, 0)
   mut si3 = 0
   while si3 < suffix_len2 {
      __store_item_fast(M2, si3, __load_item_fast(suffix_bytes, si3))
      si3 += 1
   }
   mut pi3 = 0
   while pi3 < final_pad.len {
      __store_item_fast(M2, suffix_len2 + pi3, __load_item_fast(final_pad, pi3))
      pi3 += 1
   }
   def num_blocks3 = padded_suffix_len2 / 64
   mut bi3 = 0
   mut a = h0
   mut b = h1
   mut c = h2
   mut d = h3
   mut e = h4
   mut f = h5
   mut g = h6
   mut h = h7
   mut W2 = list(64)
   store64(W2, 64, 0)
   while bi3 < num_blocks3 {
      mut wi2 = 0
      while wi2 < 16 {
         def base2 = bi3 * 64 + wi2 * 4
         def word2 = (__load_item_fast(M2, base2) << 24) | (__load_item_fast(M2, base2 + 1) << 16) | (__load_item_fast(M2, base2 + 2) << 8) | __load_item_fast(M2, base2 + 3)
         __store_item_fast(W2, wi2, _u32(word2))
         wi2 += 1
      }
      while wi2 < 64 {
         def wm15 = __load_item_fast(W2, wi2 - 15)
         def wm2 = __load_item_fast(W2, wi2 - 2)
         def s0 = _rotr32(wm15, 7) ^^ _rotr32(wm15, 18) ^^ (wm15 >> 3)
         def s1 = _rotr32(wm2, 17) ^^ _rotr32(wm2, 19) ^^ (wm2 >> 10)
         def w2 = _u32(__load_item_fast(W2, wi2 - 16) + s0 + __load_item_fast(W2, wi2 - 7) + s1)
         __store_item_fast(W2, wi2, w2)
         wi2 += 1
      }
      mut aa3 = a
      mut bb3 = b
      mut cc3 = c
      mut dd3 = d
      mut ee3 = e
      mut ff3 = f
      mut gg3 = g
      mut hh3 = h
      mut ri3 = 0
      while ri3 < 64 {
         def S1 = _rotr32(ee3, 6) ^^ _rotr32(ee3, 11) ^^ _rotr32(ee3, 25)
         def ch = (ee3 & ff3) ^^ ((ee3 ^^ 4294967295) & gg3)
         def t1 = _u32(hh3 + S1 + ch + __load_item_fast(_SHA256_K, ri3) + __load_item_fast(W2, ri3))
         def S0 = _rotr32(aa3, 2) ^^ _rotr32(aa3, 13) ^^ _rotr32(aa3, 22)
         def maj = (aa3 & bb3) ^^ (aa3 & cc3) ^^ (bb3 & cc3)
         def t2 = _u32(S0 + maj)
         hh3 = gg3
         gg3 = ff3
         ff3 = ee3
         ee3 = _u32(dd3 + t1)
         dd3 = cc3
         cc3 = bb3
         bb3 = aa3
         aa3 = _u32(t1 + t2)
         ri3 += 1
      }
      a, b = _u32(a + aa3), _u32(b + bb3)
      c, d = _u32(c + cc3), _u32(d + dd3)
      e, f = _u32(e + ee3), _u32(f + ff3)
      g, h = _u32(g + gg3), _u32(h + hh3)
      bi3 += 1
   }
   _u32_to_hex_be(a) + _u32_to_hex_be(b) + _u32_to_hex_be(c) + _u32_to_hex_be(d) +
   _u32_to_hex_be(e) + _u32_to_hex_be(f) + _u32_to_hex_be(g) + _u32_to_hex_be(h)
}

def _U64_MASK_LE = bigint_from_str("18446744073709551615")
def _U64_ZERO_LE = bigint_from_str("0")
def _K512_LE = [
   0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc,
   0x3956c25bf348b538, 0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118,
   0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2,
   0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235, 0xc19bf174cf692694,
   0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65,
   0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
   0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4,
   0xc6e00bf33da88fc2, 0xd5a79147930aa725, 0x06ca6351e003826f, 0x142929670a0e6e70,
   0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df,
   0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
   0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30,
   0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
   0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8,
   0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3,
   0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
   0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b,
   0xca273eceea26619c, 0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178,
   0x06f067aa72176fba, 0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
   0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c,
   0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817
]

fn _u64_word_le(any x) any { (x & _U64_MASK_LE) + _U64_ZERO_LE }

fn _u64_list_le(int n) list {
   mut out = list(n)
   store64(out, n, 0)
   mut i = 0
   while i < n {
      __store_item_fast(out, i, _U64_ZERO_LE)
      i += 1
   }
   out
}

fn _rotr64_le(any x, int n) any {
   def shift = n % 64
   def word = _u64_word_le(x)
   if shift == 0 { return word }
   _u64_word_le((word >> shift) | (word << (64 - shift)))
}

fn _sha512_ch(any x, any y, any z) any {
   def ux, uy = _u64_word_le(x), _u64_word_le(y)
   def uz = _u64_word_le(z)
   _u64_word_le((ux & uy) ^^ ((_U64_MASK_LE ^^ ux) & uz))
}

fn _sha512_maj(any x, any y, any z) any {
   def ux, uy = _u64_word_le(x), _u64_word_le(y)
   def uz = _u64_word_le(z)
   _u64_word_le((ux & uy) ^^ (ux & uz) ^^ (uy & uz))
}

fn _sha512_big0(any x) any { _u64_word_le(_rotr64_le(x, 28) ^^ _rotr64_le(x, 34) ^^ _rotr64_le(x, 39)) }

fn _sha512_big1(any x) any { _u64_word_le(_rotr64_le(x, 14) ^^ _rotr64_le(x, 18) ^^ _rotr64_le(x, 41)) }

fn _sha512_small0(any x) any {
   def word = _u64_word_le(x)
   _u64_word_le(_rotr64_le(word, 1) ^^ _rotr64_le(word, 8) ^^ (word >> 7))
}

fn _sha512_small1(any x) any {
   def word = _u64_word_le(x)
   _u64_word_le(_rotr64_le(word, 19) ^^ _rotr64_le(word, 61) ^^ (word >> 6))
}

fn _u64_from_bytes_be(list bs, int offset) any {
   mut out = _U64_ZERO_LE
   mut i = 0
   while i < 8 {
      out = (out * 256) + int(__load_item_fast(bs, offset + i))
      i += 1
   }
   _u64_word_le(out)
}

fn _u64_to_hex_be(any v) str {
   def word = _u64_word_le(v)
   mut out = list(8)
   store64(out, 8, 0)
   mut i = 7
   while i >= 0 {
      __store_item_fast(out, 7 - i, bigint_to_int((word >> (i * 8)) & 255))
      i -= 1
   }
   out.hex
}

fn sha512_length_extend(str orig_hash_hex, int orig_len_bytes, list suffix_bytes) str {
   "Perform SHA-512 length extension attack.
   Given a valid SHA-512 hash of an unknown message,
   compute a valid SHA-512 hash of(original_message || padding || suffix)."
   def suffix_len = suffix_bytes.len
   def glue = _sha_padding(orig_len_bytes, 128, 16)
   def final_pad = _sha_padding(orig_len_bytes + glue.len + suffix_len, 128, 16)
   def hash_bytes = orig_hash_hex.unhex
   mut h = _u64_list_le(8)
   mut hi = 0
   while hi < 8 {
      h[hi] = _u64_from_bytes_be(hash_bytes, hi * 8)
      hi += 1
   }
   def padded_suffix_len = suffix_len + final_pad.len
   mut M = list(padded_suffix_len)
   store64(M, padded_suffix_len, 0)
   mut si = 0
   while si < suffix_len {
      __store_item_fast(M, si, __load_item_fast(suffix_bytes, si))
      si += 1
   }
   mut pi = 0
   while pi < final_pad.len {
      __store_item_fast(M, suffix_len + pi, __load_item_fast(final_pad, pi))
      pi += 1
   }
   mut off = 0
   while off < padded_suffix_len {
      mut w = _u64_list_le(80)
      mut wi = 0
      while wi < 16 {
         w[wi] = _u64_from_bytes_be(M, off + wi * 8)
         wi += 1
      }
      while wi < 80 {
         w[wi] = _u64_word_le(_sha512_small1(w[wi - 2]) + w[wi - 7] + _sha512_small0(w[wi - 15]) + w[wi - 16])
         wi += 1
      }
      mut a, b = h[0], h[1]
      mut c, d = h[2], h[3]
      mut e, f = h[4], h[5]
      mut g = h[6]
      mut hh = h[7]
      mut ri = 0
      while ri < 80 {
         def t1 = _u64_word_le(hh + _sha512_big1(e) + _sha512_ch(e, f, g) + _K512_LE[ri] + w[ri])
         def t2 = _u64_word_le(_sha512_big0(a) + _sha512_maj(a, b, c))
         hh = g
         g = f
         f = e
         e = _u64_word_le(d + t1)
         d = c
         c = b
         b = a
         a = _u64_word_le(t1 + t2)
         ri += 1
      }
      h[0] = _u64_word_le(h[0] + a)
      h[1] = _u64_word_le(h[1] + b)
      h[2] = _u64_word_le(h[2] + c)
      h[3] = _u64_word_le(h[3] + d)
      h[4] = _u64_word_le(h[4] + e)
      h[5] = _u64_word_le(h[5] + f)
      h[6] = _u64_word_le(h[6] + g)
      h[7] = _u64_word_le(h[7] + hh)
      off += 128
   }
   _u64_to_hex_be(h[0]) + _u64_to_hex_be(h[1]) + _u64_to_hex_be(h[2]) + _u64_to_hex_be(h[3]) +
   _u64_to_hex_be(h[4]) + _u64_to_hex_be(h[5]) + _u64_to_hex_be(h[6]) + _u64_to_hex_be(h[7])
}

fn _sha_padding(int msg_len_bytes, int block_size, int len_field_bytes) list {
   "Internal: Compute SHA-style padding for message of given byte length.
   Appends 0x80, then zeros, then big-endian length in bits.
   Returns padding bytes as a list."
   mut pad_len = block_size - (msg_len_bytes % block_size)
   if pad_len <= len_field_bytes { pad_len = pad_len + block_size }
   mut padding = list(pad_len)
   store64(padding, pad_len, 0)
   __store_item_fast(padding, 0, 128)
   def msg_len_bits = msg_len_bytes * 8
   mut j = 0
   if len_field_bytes == 16 {
      j = 8
      while j < 16 {
         __store_item_fast(padding, pad_len - 16 + j, (msg_len_bits >> ((15 - j) * 8)) & 255)
         j += 1
      }
   } else {
      while j < len_field_bytes {
         __store_item_fast(padding, pad_len - len_field_bytes + j, (msg_len_bits >> ((len_field_bytes - 1 - j) * 8)) & 255)
         j += 1
      }
   }
   padding
}

fn _u32(int x) int { x & 4294967295 }

fn _rotl32(int x, int n) int {
   def v = _u32(x)
   _u32((v << n) | (v >> (32 - n)))
}

fn _rotr32(int x, int n) int {
   def v = _u32(x)
   _u32((v >> n) | (v << (32 - n)))
}

fn _u32_from_bytes_le(list bs, int offset) int {
   def b0, b1 = __load_item_fast(bs, offset), __load_item_fast(bs, offset + 1)
   def b2, b3 = __load_item_fast(bs, offset + 2), __load_item_fast(bs, offset + 3)
   b0 | (b1 << 8) | (b2 << 16) | (b3 << 24)
}

fn _u32_from_bytes_be(list bs, int offset) int {
   def b0, b1 = __load_item_fast(bs, offset), __load_item_fast(bs, offset + 1)
   def b2, b3 = __load_item_fast(bs, offset + 2), __load_item_fast(bs, offset + 3)
   (b0 << 24) | (b1 << 16) | (b2 << 8) | b3
}

fn _u32_to_hex_le(int n) str {
   def v = _u32(n)
   [v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >> 24) & 255].hex
}

fn _u32_to_hex_be(int n) str {
   def v = _u32(n)
   [(v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255].hex
}
