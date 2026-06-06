;; Keywords: hash digest hmac md4 md5 sha1 sha256 sha512 sha3 blake2s ripemd160 crc32 adler32 fnv xxhash ntlm password length-extension math crypto
;; Cryptography hash helpers for algorithms, analysis, validation, or supporting math.
;; References:
;; - https://www.rfc-editor.org/rfc/rfc1321
;; - https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.180-4.pdf
module std.math.crypto.hash(crc32, adler32, fnv1a, xxh32, sha1, md5, md5_bytes, sha256, sha512, ripemd160_bytes, ripemd160_hex, sha3_256, sha3_512, blake2s, sha256_hex, sha256_bytes, sha256_hmac, md5_hmac, md4_bytes, md4_hex, ntlm_hash, dcc_hash, dcc2_hash, native_available, native_backend_name, native_last_error, md5_padding, sha1_padding, sha256_padding, birthday, rainbow, length_extension, hash_dict_crack, hash_brute_crack)
use std.core
use std.core.mem (zalloc, zalloc_size, malloc, free, memcpy, __copy_mem)
use std.math.bin (
   _u32, _and32, _or32, _xor32, _not32, _add32, _lshr32, _rotl32,
   _load_le32, rotr, zero_list,
)

use std.math.big (bigint_from_str, bigint_to_int)
use std.math.simmd as simmd
use std.core.str as str
use std.math.crypto.hash.ntlm
use std.core.common as common

if(comptime{ __os_name() == "linux" }){
   #link "libcrypto.so"
   #include <openssl/evp.h> as "EVP_"
   #include <openssl/hmac.h> as "HMAC_"
}

if(comptime{ __os_name() == "windows" }){
   #link "libcrypto.lib"
   #include <openssl/evp.h> as "EVP_"
   #include <openssl/hmac.h> as "HMAC_"
}

if(comptime{ __os_name() == "macos" }){
   #link "libcrypto.dylib"
   #include <openssl/evp.h> as "EVP_"
   #include <openssl/hmac.h> as "HMAC_"
}

extern "crypto" {
   fn _EVP_MD_CTX_new() handle as "EVP_MD_CTX_new"
   fn _EVP_MD_CTX_free(handle ctx) as "EVP_MD_CTX_free"
   fn _EVP_get_digestbyname(str name) handle as "EVP_get_digestbyname"
   fn _EVP_DigestInit_ex(handle ctx, handle typ, handle impl) i32 as "EVP_DigestInit_ex"
   fn _EVP_DigestUpdate(handle ctx, ptr data, u64 count) i32 as "EVP_DigestUpdate"
   fn _EVP_DigestFinal_ex(handle ctx, ptr md, ptr size) i32 as "EVP_DigestFinal_ex"
   fn _HMAC(handle evp_md, ptr key, i32 key_len, ptr data, u64 data_len, ptr md, ptr md_len) handle as "HMAC"
}

mut _hash_native_checked = false
mut _hash_native_ok = false
mut _hash_native_backend_name = ""
mut _hash_native_last_error = ""

fn _hash_norm_span(any s, int start, int span_len) list {
   if(!is_int(start)){ start = 0 }
   if(start < 0){ start = 0 }
   def n = (is_str(s) || is_bytes(s) || is_list(s)) ? s.len : 0
   if(start > n){ start = n }
   if(!is_int(span_len) || span_len <= 0){ span_len = n - start }
   if(start + span_len > n){ span_len = n - start }
   [start, span_len]
}

fn _hash_native_set_error(any msg) bool {
   _hash_native_last_error = to_str(msg)
   false
}

fn native_available() bool {
   "Returns whether the optional native OpenSSL hash backend is available."
   _hash_native_load()
}

fn native_backend_name() str {
   "Returns the active native hash backend name, or empty string."
   _hash_native_backend_name
}

fn native_last_error() str {
   "Returns the last native hash backend initialization error."
   _hash_native_last_error
}

fn _hash_native_enabled() bool { common.env_toggle("NY_HASH_NATIVE", false) }

fn _hash_input_bytes(any data, int start=0, int span_len=0) list {
   def span = _hash_norm_span(data, start, span_len)
   start, span_len = span.get(0), span.get(1)
   mut out = []
   if(is_str(data) || is_bytes(data)){
      mut i = 0
      while(i < span_len){
         out = out.append(load8(data, start + i))
         i += 1
      }
      return out
   }
   if(is_list(data)){
      mut i = 0
      while(i < span_len){
         out = out.append(int(data[start + i]) & 255)
         i += 1
      }
      return out
   }
   out
}

fn _hash_data_bytes(any data) list {
   if(is_str(data) || is_bytes(data)){
      def n = data.len
      mut out = list(n)
      __list_set_len(out, n)
      mut i = 0
      while(i < n){
         out[i] = load8(data, i)
         i += 1
      }
      return out
   }
   if(is_list(data)){
      mut out = list(data.len)
      __list_set_len(out, data.len)
      mut i = 0
      while(i < data.len){
         out[i] = int(data[i]) & 255
         i += 1
      }
      return out
   }
   []
}

fn _hash_byte(any v) int { int(v) & 255 }

fn _hash_le32(list data, int i) int {
   _hash_byte(data.get(i, 0)) |
   (_hash_byte(data.get(i + 1, 0)) << 8) |
   (_hash_byte(data.get(i + 2, 0)) << 16) |
   (_hash_byte(data.get(i + 3, 0)) << 24)
}

fn _hash_mul32(int a, int b) int {
   def aa = _u32(a)
   def bb = _u32(b)
   def alo = aa & 65535
   def ahi = aa >> 16
   def blo = bb & 65535
   def bhi = bb >> 16
   _u32((alo * blo) + (((ahi * blo) + (alo * bhi)) << 16))
}

fn _hash_native_load() bool {
   if(_hash_native_checked){ return _hash_native_ok }
   _hash_native_checked = true
   _hash_native_ok = false
   _hash_native_backend_name = ""
   _hash_native_last_error = ""
   if(!_hash_native_enabled()){ return _hash_native_set_error("disabled by NY_HASH_NATIVE") }
   if(!(
         comptime{ __os_name() == "linux" } ||
         comptime{ __os_name() == "macos" } ||
         comptime{ __os_name() == "windows" }
      )){
      return _hash_native_set_error("native hash backend unsupported on this OS")
   }
   def handle: probe_ctx = _EVP_MD_CTX_new()
   def handle: probe_md = _EVP_get_digestbyname("SHA256")
   if(probe_ctx == 0 || probe_md == 0){
      if(probe_ctx != 0){ _EVP_MD_CTX_free(probe_ctx) }
      return _hash_native_set_error("OpenSSL EVP backend missing required symbols")
   }
   _EVP_MD_CTX_free(probe_ctx)
   _hash_native_backend_name = "openssl-evp"
   _hash_native_last_error = ""
   _hash_native_ok = true
   true
}

fn _hash_native_digest_obj(list names) handle {
   if(!_hash_native_load()){ return 0 }
   mut i = 0
   while(i < names.len){
      def handle: md = _EVP_get_digestbyname(names[i])
      if(md){ return md }
      i += 1
   }
   0
}

fn _hash_span_buf(any s, int start, int span_len) list {
   def span = _hash_norm_span(s, start, span_len)
   start = span[0]
   def slen = span[1]
   if(slen == 0){ return [0, 0, false] }
   if(is_str(s) || is_bytes(s)){ return [ptr_add(s, start), slen, false] }
   def buf = malloc(slen)
   if(!buf){ return [0, 0, false] }
   mut i = 0
   while(i < slen){
      store8(buf, (is_str(s) || is_bytes(s)) ? load8(s, start + i) : (int(s.get(start + i)) & 255), i)
      i += 1
   }
   [buf, slen, true]
}

fn _hash_bytes_from_ptr(any p, int n) list {
   mut out = list(n)
   __list_set_len(out, n)
   mut i = 0
   while(i < n){
      __store_item_fast(out, i, load8(p, i))
      i += 1
   }
   out
}

fn _hash_native_digest_hex(list names, any s, int start=0, int count=0) any {
   def out = _hash_native_digest_bytes(names, s, start, count)
   if(out == nil){ return nil }
   out.hex
}

fn _hash_native_digest_bytes(list names, any s, int start=0, int count=0) any {
   def md = _hash_native_digest_obj(names)
   if(!md){ return nil }
   def src = _hash_span_buf(s, start, count)
   def buf = src[0]
   def slen = src[1]
   def owned = src[2]
   if(!buf && slen > 0){ return nil }
   def handle: ctx = _EVP_MD_CTX_new()
   def out = malloc(128)
   def out_len_p = malloc(8)
   if(!ctx || !out || !out_len_p){
      if(owned){ free(buf) }
      if(ctx){ _EVP_MD_CTX_free(ctx) }
      free(out, out_len_p)
      return nil
   }
   store32(out_len_p, 0, 0)
   if(_EVP_DigestInit_ex(ctx, md, 0) != 1){
      if(owned){ free(buf) }
      _EVP_MD_CTX_free(ctx)
      free(out, out_len_p)
      return nil
   }
   if(slen > 0 && _EVP_DigestUpdate(ctx, buf, slen) != 1){
      if(owned){ free(buf) }
      _EVP_MD_CTX_free(ctx)
      free(out, out_len_p)
      return nil
   }
   if(_EVP_DigestFinal_ex(ctx, out, out_len_p) != 1){
      if(owned){ free(buf) }
      _EVP_MD_CTX_free(ctx)
      free(out, out_len_p)
      return nil
   }
   def out_len = load32(out_len_p, 0)
   def res = _hash_bytes_from_ptr(out, out_len)
   if(owned){ free(buf) }
   _EVP_MD_CTX_free(ctx)
   free(out, out_len_p)
   res
}

fn _hash_native_hmac_hex(list names, any key, any data) any {
   def out = _hash_native_hmac_bytes(names, key, data)
   if(out == nil){ return nil }
   out.hex
}

fn _hash_native_hmac_bytes(list names, any key, any data) any {
   def md = _hash_native_digest_obj(names)
   if(!md || !_hash_native_load()){ return nil }
   def key_src = _hash_span_buf(key, 0, 0)
   def data_src = _hash_span_buf(data, 0, 0)
   def key_buf = key_src[0]
   def key_len = key_src[1]
   def key_owned = key_src[2]
   def data_buf = data_src[0]
   def data_len = data_src[1]
   def data_owned = data_src[2]
   def out = malloc(128)
   def out_len_p = malloc(8)
   if(!out || !out_len_p){
      if(key_owned){ free(key_buf) }
      if(data_owned){ free(data_buf) }
      free(out, out_len_p)
      return nil
   }
   store32(out_len_p, 0, 0)
   if(!_HMAC(md, key_buf, key_len, data_buf, data_len, out, out_len_p)){
      if(key_owned){ free(key_buf) }
      if(data_owned){ free(data_buf) }
      free(out, out_len_p)
      return nil
   }
   def out_len = load32(out_len_p, 0)
   def res = _hash_bytes_from_ptr(out, out_len)
   if(key_owned){ free(key_buf) }
   if(data_owned){ free(data_buf) }
   free(out, out_len_p)
   res
}

def _U64_MASK = bigint_from_str("18446744073709551615")
def _U64_ZERO = bigint_from_str("0")

fn _u64_list(int n) list {
   mut out = list(n)
   mut i = 0
   while(i < n){
      out = out.append(_U64_ZERO)
      i += 1
   }
   out
}

fn _u64_word(any x) any { (x & _U64_MASK) + _U64_ZERO }

fn _rotr64(any x, int n) any {
   def shift = n % 64
   def word = _u64_word(x)
   if(shift == 0){ return word }
   _u64_word((word >> shift) | (word << (64 - shift)))
}

fn _Ch(any x, any y, any z) any {
   def ux, uy = _u64_word(x), _u64_word(y)
   def uz = _u64_word(z)
   _u64_word((ux & uy) ^^ ((_U64_MASK ^^ ux) & uz))
}

fn _Maj(any x, any y, any z) any {
   def ux, uy = _u64_word(x), _u64_word(y)
   def uz = _u64_word(z)
   _u64_word((ux & uy) ^^ (ux & uz) ^^ (uy & uz))
}

fn _Sigma0_512(any x) any {
   def word = _u64_word(x)
   _u64_word(_rotr64(word, 28) ^^ _rotr64(word, 34) ^^ _rotr64(word, 39))
}

fn _Sigma1_512(any x) any {
   def word = _u64_word(x)
   _u64_word(_rotr64(word, 14) ^^ _rotr64(word, 18) ^^ _rotr64(word, 41))
}

fn _sigma0_512(any x) any {
   def word = _u64_word(x)
   _u64_word(_rotr64(word, 1) ^^ _rotr64(word, 8) ^^ (word >> 7))
}

fn _sigma1_512(any x) any {
   def word = _u64_word(x)
   _u64_word(_rotr64(word, 19) ^^ _rotr64(word, 61) ^^ (word >> 6))
}

def _K512 = [0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc, 0x3956c25bf348b538, 0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118, 0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2, 0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235, 0xc19bf174cf692694, 0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65, 0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5, 0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4, 0xc6e00bf33da88fc2, 0xd5a79147930aa725, 0x06ca6351e003826f, 0x142929670a0e6e70, 0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df, 0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b, 0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30, 0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8, 0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8, 0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3, 0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec, 0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b, 0xca273eceea26619c, 0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178, 0x06f067aa72176fba, 0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b, 0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c, 0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817]

fn _dl64(any s, int i) any {
   mut out = _U64_ZERO
   mut j = 0
   while(j < 8){
      out = (out * 256) + load8(s, i + j)
      j += 1
   }
   _u64_word(out)
}

fn _ts64(any s, int i, any v) any {
   mut j = 7
   mut val = v
   while(j >= 0){
      store8(s, val & 255, i + j)
      val = val >> 8
      j -= 1
   }
}

fn _u64_hex(any v) str {
   def word = _u64_word(v)
   mut out = str.Builder(16)
   mut i = 7
   while(i >= 0){
      out = str.builder_append(out, str.to_hex(bigint_to_int((word >> (i * 8)) & 255), 2))
      i -= 1
   }
   def text = str.builder_to_str(out)
   str.builder_free(out)
   text
}

fn sha512(any msg, int start=0, int count=0) str {
   "Computes the SHA-512 hash of a message."
   if(_hash_native_load()){
      def native = _hash_native_digest_hex(["SHA512", "sha512", "SHA-512"], msg, start, count)
      if(native != nil){ return native }
   }
   mut data = _hash_input_bytes(msg, start, count)
   def n = data.len
   mut h = [
      0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
      0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179
   ]
   def padding_len = (n % 128 < 112) ? (128 - (n % 128)) : (256 - (n % 128))
   def total_len = n + padding_len
   mut m = malloc(total_len)
   mut bi = 0
   while(bi < n){
      store8(m, data.get(bi), bi)
      bi += 1
   }
   store8(m, 128, n)
   mut i = n + 1
   while(i < total_len - 16){ store8(m, 0, i) i += 1 }
   while(i < total_len - 8){ store8(m, 0, i) i += 1 }
   _ts64(m, total_len - 8, n * 8)
   mut p = 0
   while(p < total_len){
      mut w, j = _u64_list(80), 0
      while(j < 16){
         w[j] = _dl64(m, p + j * 8)
         j += 1
      }
      while(j < 80){
         def s0, s1 = _sigma0_512(w.get(j - 15)), _sigma1_512(w.get(j - 2))
         w[j] = _u64_word(s1 + w.get(j - 7) + s0 + w.get(j - 16))
         j += 1
      }
      mut a, b = h.get(0), h.get(1)
      mut c, d = h.get(2), h.get(3)
      mut e, f = h.get(4), h.get(5)
      mut g = h.get(6)
      mut ha = h.get(7)
      mut k = 0
      while(k < 80){
         def t1 = _u64_word(ha + _Sigma1_512(e) + _Ch(e, f, g) + _K512.get(k) + w.get(k))
         def t2 = _u64_word(_Sigma0_512(a) + _Maj(a, b, c))
         ha = g
         g = f
         f = e
         e = _u64_word(d + t1)
         d = c
         c = b
         b = a
         a = _u64_word(t1 + t2)
         k += 1
      }
      h[0] = _u64_word(h.get(0) + a)
      h[1] = _u64_word(h.get(1) + b)
      h[2] = _u64_word(h.get(2) + c)
      h[3] = _u64_word(h.get(3) + d)
      h[4] = _u64_word(h.get(4) + e)
      h[5] = _u64_word(h.get(5) + f)
      h[6] = _u64_word(h.get(6) + g)
      h[7] = _u64_word(h.get(7) + ha)
      p += 128
   }
   free(m)
   mut res = str.Builder(136)
   mut l = 0
   while(l < 8){
      res = str.builder_append(res, _u64_hex(h.get(l)))
      l += 1
   }
   def hex_text = str.builder_to_str(res)
   str.builder_free(res)
   hex_text
}

fn _bit(any v) int { int(v) }

fn crc32(any s, int start=0, int count=0) int {
   "Calculates the CRC32(Cyclic Redundancy Check) checksum of a string or buffer."
   def data = _hash_input_bytes(s, start, count)
   def slen = data.len
   mut c, i = 4294967295, 0
   while(i < slen){
      c = (c ^^ data.get(i))
      mut j = 0
      while(j < 8){
         if((c & 1) != 0){ c = (_lshr32(c, 1) ^^ 3988292384) } else { c = _lshr32(c, 1) }
         j += 1
      }
      i += 1
   }
   _u32((c ^^ 4294967295))
}

fn adler32(any s, int start=0, int count=0) int {
   "Calculates the Adler-32 checksum of a string or buffer."
   def data = _hash_input_bytes(s, start, count)
   def slen = data.len
   mut a, b = 1, 0
   mut i = 0
   while(i < slen){
      a = (a + _bit(data.get(i))) % 65521
      b = (b + a) % 65521
      i += 1
   }
   _u32(((b << 16) | a))
}

fn fnv1a(any s, int start=0, int count=0) int {
   "Calculates the 32-bit FNV-1a(Fowler-Noll-Vo) hash of a string or buffer."
   def data = _hash_input_bytes(s, start, count)
   def slen = data.len
   mut h, i = 2166136261, 0
   while(i < slen){
      h = _u32(((h ^^ _bit(data.get(i))) * 16777619))
      i += 1
   }
   h
}

fn xxh32(any s, int seed=0, int start=0, int count=0) int {
   "Calculates the XXH32 hash, a fast non-cryptographic hash algorithm."
   def data = _hash_input_bytes(s, start, count)
   def slen = data.len
   def P1 = 2654435761
   def P2 = 2246822519
   def P3 = 3266489917
   def P4 = 668265263
   def P5 = 374761393
   mut h32 = 0
   mut p = 0
   if(slen >= 16){
      mut v1, v2 = _u32(seed + P1 + P2), _u32(seed + P2)
      mut v3, v4 = _u32(seed), _u32(seed - P1)
      while(p + 16 <= slen){
         v1 = _hash_mul32(_rotl32(_u32(v1 + _hash_mul32(_bit(_hash_le32(data, p)), P2)), 13), P1)
         v2 = _hash_mul32(_rotl32(_u32(v2 + _hash_mul32(_bit(_hash_le32(data, p + 4)), P2)), 13), P1)
         v3 = _hash_mul32(_rotl32(_u32(v3 + _hash_mul32(_bit(_hash_le32(data, p + 8)), P2)), 13), P1)
         v4 = _hash_mul32(_rotl32(_u32(v4 + _hash_mul32(_bit(_hash_le32(data, p + 12)), P2)), 13), P1)
         p += 16
      }
      h32 = _u32(_rotl32(v1, 1) + _rotl32(v2, 7) + _rotl32(v3, 12) + _rotl32(v4, 18))
   } else {
      h32 = _u32(seed + P5)
   }
   h32 = _u32(h32 + slen)
   while(p + 4 <= slen){
      h32 = _hash_mul32(_rotl32(_u32(h32 + _hash_mul32(_bit(_hash_le32(data, p)), P3)), 17), P4)
      p = p + 4
   }
   while(p < slen){
      h32 = _hash_mul32(_rotl32(_u32(h32 + _hash_mul32(_hash_byte(data.get(p)), P5)), 11), P1)
      p += 1
   }
   h32 = (h32 ^^ _lshr32(h32, 15))
   h32 = _hash_mul32(h32, P2)
   h32 = (h32 ^^ _lshr32(h32, 13))
   h32 = _hash_mul32(h32, P3)
   h32 = (h32 ^^ _lshr32(h32, 16))
   h32
}

fn sha1(any s, int start=0, int count=0) str {
   "Calculates the SHA-1 hash of a string or buffer. Returns the result as a 40-character hexadecimal string."
   if(_hash_native_load()){
      def native = _hash_native_digest_hex(["SHA1", "sha1", "SHA-1"], s, start, count)
      if(native != nil){ return native }
   }
   def data = _hash_input_bytes(s, start, count)
   def slen = data.len
   mut h0, h1 = 1732584193, 4023233417
   mut h2, h3 = 2562383102, 271733878
   mut h4 = 3285377520
   def p_len = ((slen + 8) / 64 + 1) * 64
   mut m = zalloc(p_len)
   mut cpi = 0
   while(cpi < slen){
      store8(m, data.get(cpi), cpi)
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
   mut w = zero_list(80)
   mut off = 0
   while(off < p_len){
      mut t = 0
      while(t < 16){
         def base_idx = off + t * 4
         def val =
         (((_bit(load8(m, base_idx)) << 24) | (_bit(load8(m, base_idx + 1)) << 16)) |
         (_bit(load8(m, base_idx + 2)) << 8)) | _bit(load8(m, base_idx + 3))
         w[t] = _u32(val)
         t += 1
      }
      while(t < 80){
         def v = _xor32(_xor32(w.get(t - 3), w.get(t - 8)), _xor32(w.get(t - 14), w.get(t - 16)))
         w[t] = _rotl32(v, 1)
         t += 1
      }
      mut ha, hb = h0, h1
      mut hc, hd = h2, h3
      mut he = h4
      mut i = 0
      while(i < 80){
         mut int: f = 0
         mut int: k = 0
         if(i < 20){
            f, k = _or32(_and32(hb, hc), _and32(_not32(hb), hd)), 1518500249
         } else if(i < 40){
            f, k = _xor32(_xor32(hb, hc), hd), 1859775393
         } else if(i < 60){
            f, k = _or32(_or32(_and32(hb, hc), _and32(hb, hd)), _and32(hc, hd)), 2400959708
         } else {
            f, k = _xor32(_xor32(hb, hc), hd), 3395469782
         }
         def temp = _u32(_rotl32(ha, 5) + f + he + k + w.get(i))
         he, hd = hd, hc
         hc, hb = _rotl32(hb, 30), ha
         ha = temp
         i += 1
      }
      h0, h1 = _u32(h0 + ha), _u32(h1 + hb)
      h2, h3 = _u32(h2 + hc), _u32(h3 + hd)
      h4 = _u32(h4 + he)
      off = off + 64
   }
   free(m)
   return str.to_hex(h0, 8) + str.to_hex(h1, 8) + str.to_hex(h2, 8) + str.to_hex(h3, 8) + str.to_hex(h4, 8)
}

fn md5(any s, int start=0, int count=0) str {
   "Calculates the MD5(Message-Digest Algorithm 5) hash of a string or buffer. " +
   "Returns a 32-character hexadecimal string."
   if(_hash_native_load()){
      def native = _hash_native_digest_hex(["MD5", "md5"], s, start, count)
      if(native != nil){ return native }
   }
   def data = _hash_input_bytes(s, start, count)
   def slen = data.len
   mut h0, h1 = 1732584193, 4023233417
   mut h2, h3 = 2562383102, 271733878
   def p_len = ((slen + 8) / 64 + 1) * 64
   mut m = zalloc(p_len)
   mut cpi = 0
   while(cpi < slen){
      store8(m, data.get(cpi), cpi)
      cpi += 1
   }
   store8(m, 128, slen)
   def bitlen = slen * 8
   store8(m, bitlen % 256, p_len - 8)
   store8(m, (bitlen / 256) % 256, p_len - 7)
   store8(m, (bitlen / 65536) % 256, p_len - 6)
   store8(m, (bitlen / 16777216) % 256, p_len - 5)
   store8(m, (bitlen / 4294967296) % 256, p_len - 4)
   store8(m, (bitlen / 1099511627776) % 256, p_len - 3)
   store8(m, (bitlen / 281474976710656) % 256, p_len - 2)
   store8(m, (bitlen / 72057594037927936) % 256, p_len - 1)
   def K = [
      3614090360, 3905402710, 606105819, 3250441966, 4118548399, 1200080426, 2821735955, 4249261313,
      1770035416, 2336552879, 4294925233, 2304563134, 1804603682, 4254626195, 2792965006, 1236535329,
      4129170786, 3225465664, 643717713, 3921069994, 3593408605, 38016083, 3634488961, 3889429448,
      568446438, 3275163606, 4107603335, 1163531501, 2850285829, 4243563512, 1735328473, 2368359562,
      4294588738, 2272392833, 1839030562, 4259657740, 2763975236, 1272893353, 4139469664, 3200236656,
      681279174, 3936430074, 3572445317, 76029189, 3654602809, 3873151461, 530742520, 3299628645,
      4096336452, 1126891415, 2878612391, 4237533241, 1700485571, 2399980690, 4293915773, 2240044497,
      1873313359, 4264355552, 2734768916, 1309151649, 4149444226, 3174756917, 718787259, 3951481745
   ]
   def S = [
      7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
      5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
      4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
      6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
   ]
   mut off = 0
   while(off < p_len){
      mut a, b = h0, h1
      mut c, d = h2, h3
      mut i = 0
      while(i < 64){
         mut int: f = 0
         mut int: g = 0
         if(i < 16){
            f, g = _or32(_and32(b, c), _and32(_not32(b), d)), i
         } else if(i < 32){
            f, g = _or32(_and32(d, b), _and32(_not32(d), c)), (5 * i + 1) % 16
         } else if(i < 48){
            f, g = _xor32(_xor32(b, c), d), (3 * i + 5) % 16
         } else {
            f, g = _xor32(c, _or32(b, _not32(d))), (7 * i) % 16
         }
         def base_idx = off + g * 4
         def M_g = _load_le32(m, base_idx)
         def round_sum = _add32(_add32(_add32(a, f), K.get(i)), M_g)
         def temp = _add32(b, _rotl32(round_sum, S.get(i)))
         a, d = d, c
         c, b = b, temp
         i += 1
      }
      h0, h1 = _u32(h0 + a), _u32(h1 + b)
      h2, h3 = _u32(h2 + c), _u32(h3 + d)
      off = off + 64
   }
   free(m)
   return str.to_hex(h0 % 256, 2) +
   str.to_hex((h0 / 256) % 256, 2) +
   str.to_hex((h0 / 65536) % 256, 2) +
   str.to_hex((h0 / 16777216) % 256, 2) +
   str.to_hex(h1 % 256, 2) +
   str.to_hex((h1 / 256) % 256, 2) +
   str.to_hex((h1 / 65536) % 256, 2) +
   str.to_hex((h1 / 16777216) % 256, 2) +
   str.to_hex(h2 % 256, 2) +
   str.to_hex((h2 / 256) % 256, 2) +
   str.to_hex((h2 / 65536) % 256, 2) +
   str.to_hex((h2 / 16777216) % 256, 2) +
   str.to_hex(h3 % 256, 2) +
   str.to_hex((h3 / 256) % 256, 2) +
   str.to_hex((h3 / 65536) % 256, 2) +
   str.to_hex((h3 / 16777216) % 256, 2)
}

fn md5_bytes(any data) list {
   "Compute MD5 and return a 16-byte list."
   md5(data).unhex
}

def SHA256_K = [0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2]

fn _sha256_rotr(int x, int n) int {
   ((x >> n) | (x << (32 - n))) & 0xffffffff
}

fn _sha256_ch(int x, int y, int z) int { (x & y) ^^ ((0xffffffff ^^ x) & z) }

fn _sha256_maj(int x, int y, int z) int { (x & y) ^^ (x & z) ^^ (y & z) }

fn sha256(any data) list {
   "Compute SHA-256 hash of message(string or bytes). Returns bytes list."
   if(_hash_native_load()){
      def native = _hash_native_digest_bytes(["SHA256", "sha256", "SHA-256"], data, 0, 0)
      if(native != nil){ return native }
   }
   mut msg_len = 0
   if(is_str(data) || is_bytes(data) || is_list(data)){ msg_len = data.len }
   mut padded_len = msg_len + 1
   while((padded_len % 64) != 56){ padded_len += 1 }
   def total_len = padded_len + 8
   def msg = zalloc(total_len)
   def w = malloc(512)
   if(!msg || !w){
      free(msg, w)
      return []
   }
   mut copy_i = 0
   if(is_str(data) || is_bytes(data)){
      while(copy_i < msg_len){
         store8(msg, load8(data, copy_i), copy_i)
         copy_i += 1
      }
   } else if(is_list(data)){
      while(copy_i < msg_len){
         store8(msg, int(data[copy_i]) & 255, copy_i)
         copy_i += 1
      }
   }
   store8(msg, 128, msg_len)
   def bit_len = msg_len * 8
   mut i = 7
   while(i >= 0){
      store8(msg, (bit_len >> (i * 8)) & 255, total_len - 8 + (7 - i))
      i -= 1
   }
   mut h0 = 0x6a09e667
   mut h1 = 0xbb67ae85
   mut h2 = 0x3c6ef372
   mut h3 = 0xa54ff53a
   mut h4 = 0x510e527f
   mut h5 = 0x9b05688c
   mut h6 = 0x1f83d9ab
   mut h7 = 0x5be0cd19
   mut chunk_start = 0
   while(chunk_start < total_len){
      i = 0
      while(i < 16){
         def idx = chunk_start + i * 4
         def word = (load8(msg, idx) << 24) | (load8(msg, idx + 1) << 16) |
         (load8(msg, idx + 2) << 8) | load8(msg, idx + 3)
         store64(w, word & 0xffffffff, i * 8)
         i += 1
      }
      while(i < 64){
         def wm15 = load64(w, (i - 15) * 8)
         def wm2 = load64(w, (i - 2) * 8)
         def s0 = _sha256_rotr(wm15, 7) ^^ _sha256_rotr(wm15, 18) ^^ (wm15 >> 3)
         def s1 = _sha256_rotr(wm2, 17) ^^ _sha256_rotr(wm2, 19) ^^ (wm2 >> 10)
         def word = load64(w, (i - 16) * 8) + s0 + load64(w, (i - 7) * 8) + s1
         store64(w, word & 0xffffffff, i * 8)
         i += 1
      }
      mut a, b = h0, h1
      mut c, d = h2, h3
      mut e, f = h4, h5
      mut g = h6
      mut hh = h7
      i = 0
      while(i < 64){
         def t1 = (hh + (_sha256_rotr(e, 6) ^^ _sha256_rotr(e, 11) ^^ _sha256_rotr(e, 25)) +
         _sha256_ch(e, f, g) + SHA256_K[i] + load64(w, i * 8)) & 0xffffffff
         def t2 = ((_sha256_rotr(a, 2) ^^ _sha256_rotr(a, 13) ^^ _sha256_rotr(a, 22)) + _sha256_maj(a, b, c)) & 0xffffffff
         hh = g
         g = f
         f = e
         e = (d + t1) & 0xffffffff
         d = c
         c = b
         b = a
         a = (t1 + t2) & 0xffffffff
         i += 1
      }
      h0 = (h0 + a) & 0xffffffff
      h1 = (h1 + b) & 0xffffffff
      h2 = (h2 + c) & 0xffffffff
      h3 = (h3 + d) & 0xffffffff
      h4 = (h4 + e) & 0xffffffff
      h5 = (h5 + f) & 0xffffffff
      h6 = (h6 + g) & 0xffffffff
      h7 = (h7 + hh) & 0xffffffff
      chunk_start += 64
   }
   free(msg, w)
   mut result = list(32)
   __list_set_len(result, 32)
   mut k = 0
   while(k < 8){
      def hi = case k {
         0 -> h0
         1 -> h1
         2 -> h2
         3 -> h3
         4 -> h4
         5 -> h5
         6 -> h6
         _ -> h7
      }
      def base = k * 4
      __store_item_fast(result, base, (hi >> 24) & 255)
      __store_item_fast(result, base + 1, (hi >> 16) & 255)
      __store_item_fast(result, base + 2, (hi >> 8) & 255)
      __store_item_fast(result, base + 3, hi & 255)
      k += 1
   }
   result
}

fn sha256_hex(any data) str {
   "Compute SHA-256 hash and return as hex string."
   sha256(data).hex
}

fn sha256_bytes(any data) list {
   "Compute SHA-256 hash. Returns bytes list.
   Accepts string, bytes, or byte-list."
   sha256(data)
}

def _RIPEMD160_R = [
   0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
   7, 4, 13, 1, 10, 6, 15, 3, 12, 0, 9, 5, 2, 14, 11, 8,
   3, 10, 14, 4, 9, 15, 8, 1, 2, 7, 0, 6, 13, 11, 5, 12,
   1, 9, 11, 10, 0, 8, 12, 4, 13, 3, 7, 15, 14, 5, 6, 2,
   4, 0, 5, 9, 7, 12, 2, 10, 14, 1, 3, 8, 11, 6, 15, 13
]

def _RIPEMD160_RP = [
   5, 14, 7, 0, 9, 2, 11, 4, 13, 6, 15, 8, 1, 10, 3, 12,
   6, 11, 3, 7, 0, 13, 5, 10, 14, 15, 8, 12, 4, 9, 1, 2,
   15, 5, 1, 3, 7, 14, 6, 9, 11, 8, 12, 2, 10, 0, 4, 13,
   8, 6, 4, 1, 3, 11, 15, 0, 5, 12, 2, 13, 9, 7, 10, 14,
   12, 15, 10, 4, 1, 5, 8, 7, 6, 2, 13, 14, 0, 3, 9, 11
]

def _RIPEMD160_S = [
   11, 14, 15, 12, 5, 8, 7, 9, 11, 13, 14, 15, 6, 7, 9, 8,
   7, 6, 8, 13, 11, 9, 7, 15, 7, 12, 15, 9, 11, 7, 13, 12,
   11, 13, 6, 7, 14, 9, 13, 15, 14, 8, 13, 6, 5, 12, 7, 5,
   11, 12, 14, 15, 14, 15, 9, 8, 9, 14, 5, 6, 8, 6, 5, 12,
   9, 15, 5, 11, 6, 8, 13, 12, 5, 12, 13, 14, 11, 8, 5, 6
]

def _RIPEMD160_SP = [
   8, 9, 9, 11, 13, 15, 15, 5, 7, 7, 8, 11, 14, 14, 12, 6,
   9, 13, 15, 7, 12, 8, 9, 11, 7, 7, 12, 7, 6, 15, 13, 11,
   9, 7, 15, 11, 8, 6, 6, 14, 12, 13, 5, 14, 13, 13, 7, 5,
   15, 5, 8, 11, 14, 14, 6, 14, 6, 9, 12, 9, 12, 5, 15, 8,
   8, 5, 12, 9, 12, 5, 14, 6, 8, 13, 6, 5, 15, 13, 11, 11
]

fn _ripemd160_f(int j, int x, int y, int z) int {
   if(j < 16){ return _xor32(_xor32(x, y), z) }
   if(j < 32){ return _or32(_and32(x, y), _and32(_not32(x), z)) }
   if(j < 48){ return _xor32(_or32(x, _not32(y)), z) }
   if(j < 64){ return _or32(_and32(x, z), _and32(y, _not32(z))) }
   _xor32(x, _or32(y, _not32(z)))
}

fn _ripemd160_k(int j) int {
   if(j < 16){ return 0x00000000 }
   if(j < 32){ return 0x5a827999 }
   if(j < 48){ return 0x6ed9eba1 }
   if(j < 64){ return 0x8f1bbcdc }
   0xa953fd4e
}

fn _ripemd160_kp(int j) int {
   if(j < 16){ return 0x50a28be6 }
   if(j < 32){ return 0x5c4dd124 }
   if(j < 48){ return 0x6d703ef3 }
   if(j < 64){ return 0x7a6d76e9 }
   0x00000000
}

fn _ripemd160_word(list msg, int off, int i) int {
   def p = off + i * 4
   _u32(msg[p] | (msg[p+1] << 8) | (msg[p+2] << 16) | (msg[p+3] << 24))
}

fn _ripemd160_append_le32(list out, int x) list {
   out = out.append(x & 255)
   out = out.append((x >> 8) & 255)
   out = out.append((x >> 16) & 255)
   out.append((x >> 24) & 255)
}

fn _ripemd160_pure(any data) list {
   mut msg = _hash_data_bytes(data)
   def bit_len = msg.len * 8
   msg = msg.append(128)
   while((msg.len % 64) != 56){ msg = msg.append(0) }
   mut i = 0
   while(i < 8){
      msg = msg.append((bit_len >> (8 * i)) & 255)
      i += 1
   }
   mut h0, h1 = 0x67452301, 0xefcdab89
   mut h2, h3 = 0x98badcfe, 0x10325476
   mut h4 = 0xc3d2e1f0
   mut off = 0
   while(off < msg.len){
      mut al, bl = h0, h1
      mut cl, dl = h2, h3
      mut el = h4
      mut ar = h0
      mut br = h1
      mut cr = h2
      mut dr = h3
      mut er = h4
      i = 0
      while(i < 80){
         def tl0 = _add32(_add32(_add32(al, _ripemd160_f(i, bl, cl, dl)), _ripemd160_word(msg, off, _RIPEMD160_R[i])), _ripemd160_k(i))
         def tl = _add32(_rotl32(tl0, _RIPEMD160_S[i]), el)
         al, el = el, dl
         dl, cl = _rotl32(cl, 10), bl
         bl = tl
         def jr = 79 - i
         def tr0 = _add32(_add32(_add32(ar, _ripemd160_f(jr, br, cr, dr)), _ripemd160_word(msg, off, _RIPEMD160_RP[i])), _ripemd160_kp(i))
         def tr = _add32(_rotl32(tr0, _RIPEMD160_SP[i]), er)
         ar, er = er, dr
         dr, cr = _rotl32(cr, 10), br
         br = tr
         i += 1
      }
      def t = _add32(_add32(h1, cl), dr)
      h1, h2 = _add32(_add32(h2, dl), er), _add32(_add32(h3, el), ar)
      h3, h4 = _add32(_add32(h4, al), br), _add32(_add32(h0, bl), cr)
      h0 = t
      off += 64
   }
   mut out = []
   out = _ripemd160_append_le32(out, h0)
   out = _ripemd160_append_le32(out, h1)
   out = _ripemd160_append_le32(out, h2)
   out = _ripemd160_append_le32(out, h3)
   _ripemd160_append_le32(out, h4)
}

fn ripemd160_bytes(any data) list {
   "Compute RIPEMD-160 hash of message(string or bytes). Returns bytes list.
   Uses native OpenSSL when available, then a pure-Ny fallback."
   if(_hash_native_load()){
      def out = _hash_native_digest_bytes(["RIPEMD160", "ripemd160", "RIPEMD-160"], data, 0, 0)
      if(out != nil){ return out }
   }
   _ripemd160_pure(data)
}

fn ripemd160_hex(any data) str {
   "Compute RIPEMD-160 hash and return as hex string."
   ripemd160_bytes(data).hex
}

def _KECCAK_RC = [
   0x0000000000000001, 0x0000000000008082, 0x800000000000808a, 0x8000000080008000,
   0x000000000000808b, 0x0000000080000001, 0x8000000080008081, 0x8000000000008009,
   0x000000000000008a, 0x0000000000000088, 0x0000000080008009, 0x000000008000000a,
   0x000000008000808b, 0x800000000000008b, 0x8000000000008089, 0x8000000000008003,
   0x8000000000008002, 0x8000000000000080, 0x000000000000800a, 0x800000008000000a,
   0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008
]

def _KECCAK_RHO = [
   0, 1, 62, 28, 27, 36, 44, 6, 55, 20, 3, 10, 43, 25, 39, 41, 45, 15, 21, 8, 18, 2, 61, 56, 14
]

def _KECCAK_PI = [
   0, 10, 20, 5, 15, 16, 1, 11, 21, 6, 7, 17, 2, 12, 22, 23, 8, 18, 3, 13, 14, 24, 9, 19, 4
]

fn _keccak_f1600(list st) any {
   mut i = 0
   while(i < 24){
      mut c, x = _u64_list(0), 0 while(x < 5){
         c = c.append(st.get(x) ^^ st.get(x+5) ^^ st.get(x+10) ^^ st.get(x+15) ^^ st.get(x+20))
         x += 1
      }
      mut d = _u64_list(0)
      x = 0 while(x < 5){
         def v = _u64_word(c.get((x+4)%5) ^^ _rotr64(c.get((x+1)%5), 63))
         d = d.append(v)
         x += 1
      }
      x = 0 while(x < 25){
         st[x] = _u64_word(st.get(x) ^^ d.get(x%5))
         x += 1
      }
      mut st_new = _u64_list(25)
      x = 0 while(x < 25){
         def idx = _KECCAK_PI.get(x)
         st_new[idx] = _rotr64(st.get(x), 64 - _KECCAK_RHO.get(x))
         x += 1
      }
      x = 0 while(x < 5){
         mut y = 0 while(y < 5){
            def base = y*5 + x
            def v = st_new.get(base) ^^ ((_U64_MASK ^^ st_new.get(y*5 + (x+1)%5)) & st_new.get(y*5 + (x+2)%5))
            st[base] = _u64_word(v)
            y += 1
         }
         x += 1
      }
      st[0] = _u64_word(st.get(0) ^^ _KECCAK_RC.get(i))
      i += 1
   }
}

fn _sha3_common(any data, int capacity, int out_len) str {
   mut msg = _hash_data_bytes(data)
   def rate = 1600 - capacity
   def rate_bytes = rate / 8
   msg = msg.append(0x06)
   while((msg.len % rate_bytes) != (rate_bytes - 1)){ msg = msg.append(0) }
   msg = msg.append(0x80)
   mut st = _u64_list(25)
   mut off = 0
   while(off < msg.len){
      mut j = 0
      while(j < rate_bytes && (off + j) < msg.len){
         def lane_idx = j / 8
         def cur = st.get(lane_idx)
         def b = msg.get(off + j)
         st[lane_idx] = _u64_word(cur ^^ ((_U64_ZERO + b) << (8 * (j % 8))))
         j += 1
      }
      _keccak_f1600(st)
      off += rate_bytes
   }
   mut bytes_needed = out_len / 8
   mut out = str.Builder(bytes_needed * 2 + 8)
   mut k = 0
   while(k < bytes_needed){
      def lane = st.get(k / 8)
      def b = (lane >> (8 * (k % 8))) & 255
      out = str.builder_append(out, str.to_hex(b, 2))
      k += 1
   }
   def hex_text = str.builder_to_str(out)
   str.builder_free(out)
   hex_text
}

fn sha3_256(any data) str {
   "Return SHA3-256 digest as a hexadecimal string."
   if(_hash_native_load()){
      def native = _hash_native_digest_hex(["SHA3-256", "sha3-256", "SHA3_256"], data, 0, 0)
      if(native != nil){ return native }
   }
   _sha3_common(data, 512, 256)
}

fn sha3_512(any data) str {
   "Return SHA3-512 digest as a hexadecimal string."
   if(_hash_native_load()){
      def native = _hash_native_digest_hex(["SHA3-512", "sha3-512", "SHA3_512"], data, 0, 0)
      if(native != nil){ return native }
   }
   _sha3_common(data, 1024, 512)
}

def _BLAKE2S_IV = [
   0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
]

def _BLAKE2S_SIGMA = [[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15], [14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3], [11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4], [7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8], [9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13], [2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9], [12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11], [13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10], [6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5], [10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0]]

fn _blake2s_g(list v, int a, int b, int c, int d, int x, int y) any {
   mut va, vb, vc, vd = v.get(a), v.get(b), v.get(c), v.get(d)
   va = _u32(va + vb + x)
   vd = rotr(vd ^^ va, 16, 32)
   vc = _u32(vc + vd)
   vb = rotr(vb ^^ vc, 12, 32)
   va = _u32(va + vb + y)
   vd = rotr(vd ^^ va, 8, 32)
   vc = _u32(vc + vd)
   vb = rotr(vb ^^ vc, 7, 32)
   v[a] = va
   v[b] = vb
   v[c] = vc
   v[d] = vd
}

fn _blake2s_le32(any msg, int off) int { msg.get(off, 0) | (msg.get(off + 1, 0) << 8) | (msg.get(off + 2, 0) << 16) | (msg.get(off + 3, 0) << 24) }

fn blake2s(any data, any key=nil, int out_len=32) str {
   "Return BLAKE2s digest as a hexadecimal string."
   if(key == nil && out_len == 32 && _hash_native_load()){
      def native = _hash_native_digest_hex(["BLAKE2S-256", "blake2s256", "BLAKE2s256"], data, 0, 0)
      if(native != nil){ return native }
   }
   mut msg = _hash_data_bytes(data)
   def key_len = key == nil ? 0 : key.len
   mut h = clone(_BLAKE2S_IV)
   h[0] = h.get(0) ^^ 0x01010000 ^^ (key_len << 8) ^^ out_len
   if(key != nil && key_len > 0){
      mut k_pad = clone(key)
      while(k_pad.len < 64){ k_pad = k_pad.append(0) }
      msg = k_pad.extend(msg)
   }
   def n = msg.len
   mut p, t = 0, 0
   while(p < n || n == 0){
      def block_len = (n - p < 64) ? (n - p) : 64
      t += block_len
      def is_last = (p + block_len == n)
      mut m, i = list(16), 0 while(i < 16){
         m[i] = _blake2s_le32(msg, p + i*4)
         i += 1
      }
      mut v = clone(h)
      v = v.extend(_BLAKE2S_IV)
      v[12] = v.get(12) ^^ t
      if(is_last){ v[14] = v.get(14) ^^ 0xffffffff }
      mut r = 0 while(r < 10){
         def sig = _BLAKE2S_SIGMA.get(r)
         _blake2s_g(v, 0, 4, 8, 12, m.get(sig.get(0)), m.get(sig.get(1)))
         _blake2s_g(v, 1, 5, 9, 13, m.get(sig.get(2)), m.get(sig.get(3)))
         _blake2s_g(v, 2, 6, 10, 14, m.get(sig.get(4)), m.get(sig.get(5)))
         _blake2s_g(v, 3, 7, 11, 15, m.get(sig.get(6)), m.get(sig.get(7)))
         _blake2s_g(v, 0, 5, 10, 15, m.get(sig.get(8)), m.get(sig.get(9)))
         _blake2s_g(v, 1, 6, 11, 12, m.get(sig.get(10)), m.get(sig.get(11)))
         _blake2s_g(v, 2, 7, 8, 13, m.get(sig.get(12)), m.get(sig.get(13)))
         _blake2s_g(v, 3, 4, 9, 14, m.get(sig.get(14)), m.get(sig.get(15)))
         r += 1
      }
      i = 0 while(i < 8){
         h[i] = h.get(i) ^^ v.get(i) ^^ v.get(i+8)
         i += 1
      }
      p += 64
      if(n == 0){ break }
   }
   mut out = str.Builder(out_len * 2 + 8)
   mut i = 0 while(i < out_len){
      def word = h.get(i / 4)
      out = str.builder_append(out, str.to_hex((word >> (8 * (i % 4))) & 255, 2))
      i += 1
   }
   def hex_text = str.builder_to_str(out)
   str.builder_free(out)
   hex_text
}

fn md5_hmac(any key, any message) str {
   "HMAC-MD5 implementation."
   if(_hash_native_load()){
      def native = _hash_native_hmac_hex(["MD5", "md5"], key, message)
      if(native != nil){ return native }
   }
   _hash_hmac_bytes(key, message, "md5").hex
}

fn _hash_hmac_digest_bytes(str digest_name, any data) list {
   if(digest_name == "md5"){ return md5(data).unhex }
   if(digest_name == "sha256"){ return sha256(data) }
   []
}

fn _hash_hmac_bytes(any key, any data, str digest_name) list {
   def block_size = 64
   mut key_bytes = _hash_data_bytes(key)
   if(key_bytes.len > block_size){ key_bytes = _hash_hmac_digest_bytes(digest_name, key_bytes) }
   while(key_bytes.len < block_size){ key_bytes = key_bytes.append(0) }
   mut o_key_pad, i_key_pad = [], []
   mut i = 0
   while(i < block_size){
      o_key_pad, i_key_pad = o_key_pad.append(key_bytes.get(i) ^^ 0x5c), i_key_pad.append(key_bytes.get(i) ^^ 0x36)
      i += 1
   }
   mut inner = clone(i_key_pad)
   def data_bytes = _hash_data_bytes(data)
   i = 0
   while(i < data_bytes.len){
      inner = inner.append(data_bytes.get(i))
      i += 1
   }
   mut outer = clone(o_key_pad)
   def inner_hash = _hash_hmac_digest_bytes(digest_name, inner)
   i = 0
   while(i < inner_hash.len){
      outer = outer.append(inner_hash.get(i))
      i += 1
   }
   _hash_hmac_digest_bytes(digest_name, outer)
}

fn sha256_hmac(any key, any data) list {
   "HMAC-SHA256."
   if(_hash_native_load()){
      def native = _hash_native_hmac_bytes(["SHA256", "sha256", "SHA-256"], key, data)
      if(native != nil){ return native }
   }
   _hash_hmac_bytes(key, data, "sha256")
}

fn md5_padding(int msg_len) list {
   "Compute MD5 padding byte list."
   def bit_len = msg_len * 8
   def pad_len = ((55 - msg_len) % 64 + 64) % 64 + 1
   mut pad = [0x80]
   mut i = 1
   while(i < pad_len){
      pad = pad.append(0)
      i += 1
   }
   mut j = 0
   while(j < 8){
      pad = pad.append((bit_len >> (j * 8)) & 255)
      j += 1
   }
   pad
}

fn sha1_padding(int msg_len) list {
   "Compute SHA1 padding byte list."
   def bit_len = msg_len * 8
   def pad_len = ((55 - msg_len) % 64 + 64) % 64 + 1
   mut pad = [0x80]
   mut i = 1
   while(i < pad_len){
      pad = pad.append(0)
      i += 1
   }
   mut j = 7
   while(j >= 0){
      pad = pad.append((bit_len >> (j * 8)) & 255)
      j = j - 1
   }
   pad
}

fn sha256_padding(int msg_len) list {
   "Compute SHA256 padding byte list."
   sha1_padding(msg_len)
}

impl str {
   @inline
   fn md5(str s) str {
      "Return MD5 hex digest for this string."
      md5(s)
   }
   @inline
   fn sha1(str s) str {
      "Return SHA1 hex digest for this string."
      sha1(s)
   }
   @inline
   fn sha256(str s) list {
      "Return SHA256 digest bytes for this string."
      sha256(s)
   }
   @inline
   fn sha256_hex(str s) str {
      "Return SHA256 hex digest for this string."
      sha256_hex(s)
   }
   @inline
   fn sha512(str s) str {
      "Return SHA512 hex digest for this string."
      sha512(s)
   }
   @inline
   fn sha3_256(str s) str {
      "Return SHA3-256 hex digest for this string."
      sha3_256(s)
   }
   @inline
   fn sha3_512(str s) str {
      "Return SHA3-512 hex digest for this string."
      sha3_512(s)
   }
   @inline
   fn blake2s(str s) str {
      "Return BLAKE2s hex digest for this string."
      blake2s(s)
   }
   @inline
   fn ripemd160(str s) list {
      "Return RIPEMD-160 digest bytes for this string."
      ripemd160_bytes(s)
   }
   @inline
   fn ripemd160_hex(str s) str {
      "Return RIPEMD-160 hex digest for this string."
      ripemd160_hex(s)
   }
}

impl list {
   @inline
   fn md5(list b) str {
      "Return MD5 hex digest for this byte list."
      md5(b)
   }
   @inline
   fn sha1(list b) str {
      "Return SHA1 hex digest for this byte list."
      sha1(b)
   }
   @inline
   fn sha256(list b) list {
      "Return SHA256 digest bytes for this byte list."
      sha256(b)
   }
   @inline
   fn sha256_hex(list b) str {
      "Return SHA256 hex digest for this byte list."
      sha256_hex(b)
   }
   @inline
   fn sha512(list b) str {
      "Return SHA512 hex digest for this byte list."
      sha512(b)
   }
   @inline
   fn sha3_256(list b) str {
      "Return SHA3-256 hex digest for this byte list."
      sha3_256(b)
   }
   @inline
   fn sha3_512(list b) str {
      "Return SHA3-512 hex digest for this byte list."
      sha3_512(b)
   }
   @inline
   fn blake2s(list b) str {
      "Return BLAKE2s hex digest for this byte list."
      blake2s(b)
   }
   @inline
   fn ripemd160(list b) list {
      "Return RIPEMD-160 digest bytes for this byte list."
      ripemd160_bytes(b)
   }
   @inline
   fn ripemd160_hex(list b) str {
      "Return RIPEMD-160 hex digest for this byte list."
      ripemd160_hex(b)
   }
}

impl bytes {
   @inline
   fn md5(bytes b) str {
      "Return MD5 hex digest for this byte buffer."
      md5(b)
   }
   @inline
   fn sha1(bytes b) str {
      "Return SHA1 hex digest for this byte buffer."
      sha1(b)
   }
   @inline
   fn sha256(bytes b) list {
      "Return SHA256 digest bytes for this byte buffer."
      sha256(b)
   }
   @inline
   fn sha256_hex(bytes b) str {
      "Return SHA256 hex digest for this byte buffer."
      sha256_hex(b)
   }
   @inline
   fn sha512(bytes b) str {
      "Return SHA512 hex digest for this byte buffer."
      sha512(b)
   }
   @inline
   fn sha3_256(bytes b) str {
      "Return SHA3-256 hex digest for this byte buffer."
      sha3_256(b)
   }
   @inline
   fn sha3_512(bytes b) str {
      "Return SHA3-512 hex digest for this byte buffer."
      sha3_512(b)
   }
   @inline
   fn blake2s(bytes b) str {
      "Return BLAKE2s hex digest for this byte buffer."
      blake2s(b)
   }
   @inline
   fn ripemd160(bytes b) list {
      "Return RIPEMD-160 digest bytes for this byte buffer."
      ripemd160_bytes(b)
   }
   @inline
   fn ripemd160_hex(bytes b) str {
      "Return RIPEMD-160 hex digest for this byte buffer."
      ripemd160_hex(b)
   }
}

fn hash_dict_crack(any target_hash, list wordlist, fnptr hash_fn) any {
   "Dictionary attack on hashes."
   mut i = 0
   while(i < wordlist.len){
      def word = wordlist.get(i)
      if(hash_fn(word) == target_hash){ return word }
      i += 1
   }
   nil
}

fn hash_brute_crack(any target_hash, str charset, int max_len, fnptr hash_fn) any {
   "Brute-force attack on hashes."
   def cs_len = charset.len
   mut length = 1
   while(length <= max_len){
      mut indices = []
      mut i = 0
      while(i < length){
         indices = indices.append(0)
         i += 1
      }
      mut done = false
      while(!done){
         mut candidate = ""
         mut j = 0
         while(j < length){
            def idx = indices.get(j)
            candidate = str.str_add(candidate, str.utf8_slice(charset, idx, idx + 1, 1))
            j += 1
         }
         if(hash_fn(candidate) == target_hash){ return candidate }
         mut carry = 1
         mut k = length - 1
         while(k >= 0 && carry > 0){
            def new_idx = indices.get(k) + carry
            if(new_idx >= cs_len){
               indices[k] = 0
               carry = 1
            } else {
               indices[k] = new_idx
               carry = 0
            }
            k = k - 1
         }
         if(carry > 0){ done = true }
      }
      length += 1
   }
   nil
}
