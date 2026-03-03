;; Keywords: math hash crc adler xxh32 fnv sha1 md5
;; Hashing algorithms (CRC32, Adler-32, FNV-1a, XXH32, SHA-1, MD5).

module std.math.hash (
   crc32, adler32, fnv1a,
   xxh32,
   sha1, md5, sha512
)
use std.core *
use std.core.mem (zalloc, memcpy)
use std.str as str

fn _rotr64(x, n){
   "Internal: performs a 64-bit bitwise right rotation of `x` by `n` bits."
   (x >> n) | (x << (64 - n))
}

fn _Ch(x, y, z){
   "Internal: SHA-512 Choose function."
   (x & y) ^ ((x ^ -1) & z)
}
fn _Maj(x, y, z){
   "Internal: SHA-512 Majority function."
   (x & y) ^ (x & z) ^ (y & z)
}

fn _Sigma0_512(x){
   "Internal: SHA-512 Sigma0 transformation."
   _rotr64(x, 28) ^ _rotr64(x, 34) ^ _rotr64(x, 39)
}
fn _Sigma1_512(x){
   "Internal: SHA-512 Sigma1 transformation."
   _rotr64(x, 14) ^ _rotr64(x, 18) ^ _rotr64(x, 41)
}
fn _sigma0_512(x){
   "Internal: SHA-512 sigma0 transformation."
   _rotr64(x, 1) ^ _rotr64(x, 8) ^ (x >> 7)
}
fn _sigma1_512(x){
   "Internal: SHA-512 sigma1 transformation."
   _rotr64(x, 19) ^ _rotr64(x, 61) ^ (x >> 6)
}

def _K512 = [
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

fn _dl64(s, i){
   "Internal: decodes a 64-bit big-endian integer from string `s` at index `i`."
   mut out = 0
   mut j = 0
   while(j < 8){
      out = (out << 8) | load8(s, i + j)
      j += 1
   }
   out
}

fn _ts64(s, i, v){
   "Internal: encodes a 64-bit integer `v` into string `s` at index `i` in big-endian format."
   mut j = 7
   mut val = v
   while(j >= 0){
      store8(s, val & 255, i + j)
      val = val >> 8
      j -= 1
   }
}

fn sha512(msg){
   "Computes the SHA-512 hash of a message."
   if(!is_str(msg)){ return "" }
   def n = len(msg)
   mut h = [
      0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
      0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179
   ]
   ; 1. Padding
   def padding_len = (n % 128 < 112) ? (128 - (n % 128)) : (256 - (n % 128))
   def total_len = n + padding_len
   mut m = malloc(total_len)
   __copy_mem(m, msg, n)
   store8(m, 128, n) ; 0x80
   mut i = n + 1
   while(i < total_len - 8){ store8(m, 0, i) i += 1 }
   _ts64(m, total_len - 8, n * 8)
   ; 2. Process chunks
   mut p = 0
   while(p < total_len){
      mut w = list(80)
      mut j = 0
      while(j < 16){
         store_item(w, j, _dl64(m, p + j * 8))
         j += 1
      }
      while(j < 80){
         def s0 = _sigma0_512(get(w, j - 15))
         def s1 = _sigma1_512(get(w, j - 2))
         store_item(w, j, s1 + get(w, j - 7) + s0 + get(w, j - 16))
         j += 1
      }
      mut a = get(h, 0)
      mut b = get(h, 1)
      mut c = get(h, 2)
      mut d = get(h, 3)
      mut e = get(h, 4)
      mut f = get(h, 5)
      mut g = get(h, 6)
      mut ha = get(h, 7)
      mut k = 0
      while(k < 80){
         def t1 = ha + _Sigma1_512(e) + _Ch(e, f, g) + get(_K512, k) + get(w, k)
         def t2 = _Sigma0_512(a) + _Maj(a, b, c)
         ha = g
         g = f
         f = e
         e = d + t1
         d = c
         c = b
         b = a
         a = t1 + t2
         k += 1
      }
      store_item(h, 0, get(h, 0) + a)
      store_item(h, 1, get(h, 1) + b)
      store_item(h, 2, get(h, 2) + c)
      store_item(h, 3, get(h, 3) + d)
      store_item(h, 4, get(h, 4) + e)
      store_item(h, 5, get(h, 5) + f)
      store_item(h, 6, get(h, 6) + g)
      store_item(h, 7, get(h, 7) + ha)
      p += 128
   }
   free(m)
   mut res = ""
   mut l = 0
   while(l < 8){
      res = res + str.to_hex(get(h, l), 16)
      l += 1
   }
   res
}

;; Helper for bitwise sanity
fn _bit(v){
   "Internal: Ensures a value is treated as a 64-bit integer internally for bitwise operations."
   from_int(to_int(v))
}

fn _u32(x){
   "Internal: Truncates a number to its 32-bit unsigned representation."
   return x & 4294967295
}

fn _lshr32(x, n){
   "Internal: Logical shift right for 32-bit integers."
   if(n <= 0){ return _u32(x) }
   _u32(x) >> n
}

fn _rotl32(x, n){
   "Internal: Rotates a 32-bit integer left by `n` bits."
   def v = _u32(x)
   _u32(((v << n) | _lshr32(v, (32 - n))))
}

fn _norm_span(s, start, len){
   "Internal: Normalizes start and length for string/buffer operations."
   if(!is_int(start)){ start = 0 }
   if(start < 0){ start = 0 }
   def n = str.len(s)
   if(start > n){ start = n }
   if(!is_int(len) || len <= 0){ len = n - start }
   if(start + len > n){ len = n - start }
   [start, len]
}

fn crc32(s, start=0, len=0){
   "Calculates the CRC32 (Cyclic Redundancy Check) checksum of a string or buffer."
   def span = _norm_span(s, start, len)
   start = get(span, 0)
   def slen = get(span, 1)
   mut c = 4294967295
   mut i = 0
   while(i < slen){
      c = (c ^ load8(s, start + i))
      mut j = 0
      while(j < 8){
         if((c & 1) != 0){
         c = (_lshr32(c, 1) ^ 3988292384)
         } else {
         c = _lshr32(c, 1)
         }
         j += 1
      }
      i += 1
   }
   _u32((c ^ 4294967295))
}

fn adler32(s, start=0, len=0){
   "Calculates the Adler-32 checksum of a string or buffer."
   def span = _norm_span(s, start, len)
   start = get(span, 0)
   def slen = get(span, 1)
   mut a = 1
   mut b = 0
   mut i = 0
   while(i < slen){
      a = (a + _bit(load8(s, start + i))) % 65521
      b = (b + a) % 65521
      i += 1
   }
   _u32(((b << 16) | a))
}

fn fnv1a(s, start=0, len=0){
   "Calculates the 32-bit FNV-1a (Fowler-Noll-Vo) hash of a string or buffer."
   def span = _norm_span(s, start, len)
   start = get(span, 0)
   def slen = get(span, 1)
   mut h = 2166136261
   mut i = 0
   while(i < slen){
      h = _u32(((h ^ _bit(load8(s, start + i))) * 16777619))
      i += 1
   }
   h
}

fn xxh32(s, seed=0, start=0, len=0){
   "Calculates the XXH32 hash, a fast non-cryptographic hash algorithm."
   def span = _norm_span(s, start, len)
   start = get(span, 0)
   def slen = get(span, 1)
   def P1 = 2654435761
   def P2 = 2246822519
   def P3 = 3266489917
   def P4 = 668265263
   def P5 = 374761393
   mut h32 = 0
   mut p = 0
   if(slen >= 16){
      mut v1 = _u32(seed + P1 + P2)
      mut v2 = _u32(seed + P2)
      mut v3 = _u32(seed)
      mut v4 = _u32(seed - P1)
      while(p + 16 <= slen){
         v1 = _u32((_rotl32(v1 + _u32((_bit(load32(s, start + p)) * P2)), 13) * P1))
         v2 = _u32((_rotl32(v2 + _u32((_bit(load32(s, start + p + 4)) * P2)), 13) * P1))
         v3 = _u32((_rotl32(v3 + _u32((_bit(load32(s, start + p + 8)) * P2)), 13) * P1))
         v4 = _u32((_rotl32(v4 + _u32((_bit(load32(s, start + p + 12)) * P2)), 13) * P1))
         p += 16
      }
      h32 = _u32(_rotl32(v1, 1) + _rotl32(v2, 7) + _rotl32(v3, 12) + _rotl32(v4, 18))
   } else {
      h32 = _u32(seed + P5)
   }
   h32 = _u32(h32 + slen)
   while(p + 4 <= slen){
      h32 = _u32((_rotl32(_u32(h32 + _u32((_bit(load32(s, start + p)) * P3))), 17) * P4))
      p = p + 4
   }
   while(p < slen){
      h32 = _u32((_rotl32(_u32(h32 + _u32((_bit(load8(s, start + p)) * P5))), 11) * P1))
      p += 1
   }
   h32 = (h32 ^ _lshr32(h32, 15))
   h32 = _u32((h32 * P2))
   h32 = (h32 ^ _lshr32(h32, 13))
   h32 = _u32((h32 * P3))
   h32 = (h32 ^ _lshr32(h32, 16))
   h32
}

fn sha1(s, start=0, len=0){
   "Calculates the SHA-1 hash of a string or buffer. Returns the result as a 40-character hexadecimal string."
   def span = _norm_span(s, start, len)
   start = get(span, 0)
   def slen = get(span, 1)
   mut h0 = 1732584193
   mut h1 = 4023233417
   mut h2 = 2562383102
   mut h3 = 271733878
   mut h4 = 3285377520
   def p_len = ((slen + 8) / 64 + 1) * 64
   mut m = zalloc(p_len)
   ; Use loop to copy string bytes correctly
   mut cpi = 0
   while(cpi < slen){
      store8(m, load8(s, start + cpi), cpi)
      cpi += 1
   }
   store8(m, 128, slen)
   def bitlen = slen * 8
   store8(m, ((bitlen >> 56) & 255), p_len - 8)
   store8(m, ((bitlen >> 48) & 255), p_len - 7)
   store8(m, ((bitlen >> 40) & 255), p_len - 6)
   store8(m, ((bitlen >> 32) & 255), p_len - 5)
   store8(m, ((bitlen >> 24) & 255), p_len - 4)
   store8(m, ((bitlen >> 16) & 255), p_len - 3)
   store8(m, ((bitlen >> 8) & 255), p_len - 2)
   store8(m, (bitlen & 255), p_len - 1)
   mut w = malloc(320)
   mut off = 0
   while(off < p_len){
      mut t = 0
      while(t < 16){
         def base_idx = off + t * 4
         def val = ((bor((_bit(load8(m, base_idx)) << 24), (_bit(load8(m, base_idx + 1)) << 16)) | (_bit(load8(m, base_idx + 2)) << 8)) | _bit(load8(m, base_idx + 3)))
         store32(w, to_int(_u32(val)), t * 4)
         t += 1
      }
      while(t < 80){
         def v = ((bxor(_bit(load32(w, (t - 3) * 4)), _bit(load32(w, (t - 8) * 4))) ^ _bit(load32(w, (t - 14) * 4))) ^ _bit(load32(w, (t - 16) * 4)))
         store32(w, to_int(_rotl32(v, 1)), t * 4)
         t += 1
      }
      mut ha = h0
      mut hb = h1
      mut hc = h2
      mut hd = h3
      mut he = h4
      mut i = 0
      while(i < 80){
         mut f = 0
         mut k = 0
         if(i < 20){
         f = ((hb & hc) | ((hb ^ 4294967295) & hd))
         k = 1518500249
         } else if(i < 40){
         f = ((hb ^ hc) ^ hd)
         k = 1859775393
         } else if(i < 60){
         f = (((hb & hc) | (hb & hd)) | (hc & hd))
         k = 2400959708
         } else {
         f = ((hb ^ hc) ^ hd)
         k = 3395469782
         }
         def temp = _u32(_rotl32(ha, 5) + f + he + k + _bit(load32(w, i * 4)))
         he = hd
         hd = hc
         hc = _rotl32(hb, 30)
         hb = ha
         ha = temp
         i += 1
      }
      h0 = _u32(h0 + ha)
      h1 = _u32(h1 + hb)
      h2 = _u32(h2 + hc)
      h3 = _u32(h3 + hd)
      h4 = _u32(h4 + he)
      off = off + 64
   }
   free(m)
   free(w)
   return str.to_hex(h0, 8) + str.to_hex(h1, 8) + str.to_hex(h2, 8) + str.to_hex(h3, 8) + str.to_hex(h4, 8)
}

fn md5(s, start=0, len=0){
   "Calculates the MD5 (Message-Digest Algorithm 5) hash of a string or buffer. Returns the result as a 32-character hexadecimal string."
   def span = _norm_span(s, start, len)
   start = get(span, 0)
   def slen = get(span, 1)
   mut h0 = 1732584193
   mut h1 = 4023233417
   mut h2 = 2562383102
   mut h3 = 271733878
   def p_len = ((slen + 8) / 64 + 1) * 64
   mut m = zalloc(p_len)
   mut cpi = 0
   while(cpi < slen){
      store8(m, load8(s, start + cpi), cpi)
      cpi += 1
   }
   store8(m, 128, slen)
   def bitlen = slen * 8
   store8(m, (bitlen & 255), p_len - 8)
   store8(m, ((bitlen >> 8) & 255), p_len - 7)
   store8(m, ((bitlen >> 16) & 255), p_len - 6)
   store8(m, ((bitlen >> 24) & 255), p_len - 5)
   store8(m, ((bitlen >> 32) & 255), p_len - 4)
   store8(m, ((bitlen >> 40) & 255), p_len - 3)
   store8(m, ((bitlen >> 48) & 255), p_len - 2)
   store8(m, ((bitlen >> 56) & 255), p_len - 1)
   def K = [3614090360, 3905402710, 606105819, 3250441966, 4118548399, 1200080426, 2821735955, 4249261313, 1770035416, 2336552879, 4294925233, 2304563134, 1804603682, 4254626195, 2792965006, 1236535329, 4129170786, 3225465664, 643717713, 3921069994, 3593408605, 38016083, 3634488961, 3889429448, 568446438, 3275163606, 4107603335, 1163531501, 2850285829, 4243563512, 1735328473, 2368359562, 4294588738, 2272392833, 1839030562, 4259657740, 2763975236, 1272893353, 4139469664, 3200236656, 681279174, 3936430074, 3572445317, 76029189, 3654602809, 3873151461, 530742520, 3299628645, 4096336452, 1126891415, 2878612391, 4237533241, 1700485571, 2399980690, 4293915773, 2240044497, 1873313359, 4264355552, 2734768916, 1309151649, 4149444226, 3174756917, 718787259, 3951481745]
   def S = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21]
   mut off = 0
   while(off < p_len){
      mut a = h0
      mut b = h1
      mut c = h2
      mut d = h3
      mut i = 0
      while(i < 64){
         mut f = 0
         mut g = 0
         if(i < 16){
         f = ((b & c) | ((b ^ 4294967295) & d))
         g = i
         } else if(i < 32){
         f = ((d & b) | ((d ^ 4294967295) & c))
         g = (5 * i + 1) % 16
         } else if(i < 48){
         f = ((b ^ c) ^ d)
         g = (3 * i + 5) % 16
         } else {
         f = (c ^ (b | (d ^ 4294967295)))
         g = (7 * i) % 16
         }
         def base_idx = off + g * 4
         def M_g = ((bor(load8(m, base_idx), (load8(m, base_idx + 1) << 8)) | (load8(m, base_idx + 2) << 16)) | (load8(m, base_idx + 3) << 24))
         def temp = _u32(b + _rotl32(_u32(a + f + get(K, i) + M_g), get(S, i)))
         a = d
         d = c
         c = b
         b = temp
         i += 1
      }
      h0 = _u32(h0 + a)
      h1 = _u32(h1 + b)
      h2 = _u32(h2 + c)
      h3 = _u32(h3 + d)
      off = off + 64
   }
   free(m)
   return str.to_hex((h0 & 255), 2) + str.to_hex((_lshr32(h0, 8) & 255), 2) + str.to_hex((_lshr32(h0, 16) & 255), 2) + str.to_hex((_lshr32(h0, 24) & 255), 2) + str.to_hex((h1 & 255), 2) + str.to_hex((_lshr32(h1, 8) & 255), 2) + str.to_hex((_lshr32(h1, 16) & 255), 2) + str.to_hex((_lshr32(h1, 24) & 255), 2) + str.to_hex((h2 & 255), 2) + str.to_hex((_lshr32(h2, 8) & 255), 2) + str.to_hex((_lshr32(h2, 16) & 255), 2) + str.to_hex((_lshr32(h2, 24) & 255), 2) + str.to_hex((h3 & 255), 2) + str.to_hex((_lshr32(h3, 8) & 255), 2) + str.to_hex((_lshr32(h3, 16) & 255), 2) + str.to_hex((_lshr32(h3, 24) & 255), 2)
}

if(comptime{__main()}){
   print("Testing std.math.hash...")
   use std.str *

   def s = "123456789"

   ; CRC32: 0xCBF43926 -> 3421780262
   def c = crc32(s, 0, 0)
   assert(c == 3421780262, "crc32")

   ; Adler32: 152961502
   def a = adler32(s, 0, 0)
   assert(a == 152961502, "adler32")

   ; XXH32: 2474356071
   def x = xxh32(s, 0, 0, 0)
   assert(x == 2474356071, "xxh32")

   ; MD5('123456789'): 25f9e794323b453885f5181f1b624d0b
   def m = md5(s, 0, 0)
   assert((m == "25f9e794323b453885f5181f1b624d0b"), "md5")

   ; SHA1('123456789'): d2032181892c6c0a4597019109faaaf6224f771d
   def s1 = sha1(s, 0, 0)
   assert((s1 == "d2032181892c6c0a4597019109faaaf6224f771d"), "sha1")

   ; SHA512('123456789'): 10e060933ee72c9a99738ce1f0a17387431e6792ed715ecb72e01dfdcc9abd94fa9157ea8069502b4d9136ca024f2b1c41b80c3e72620780447196695273ae84
   def s2 = sha512(s, 0, 0)
   assert((s2 == "10e060933ee72c9a99738ce1f0a17387431e6792ed715ecb72e01dfdcc9abd94fa9157ea8069502b4d9136ca024f2b1c41b80c3e72620780447196695273ae84"), "sha512")
   print("✓ std.math.hash tests passed")
}
