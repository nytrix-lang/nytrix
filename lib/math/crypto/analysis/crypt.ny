;; Unix crypt compatibility (legacy DES, MD5-crypt, SHA-256-crypt, SHA-512-crypt).
;; Use only for verifying existing hashes; prefer modern password hash schemes for new credentials.
module core.math.crypto.analysis.crypt(crypt_decode6, crypt_encode6, crypt_des, crypt_md5, crypt_sha256, crypt_sha512, crypt_hash)
use core
use core.str as str
use core.math.crypto.hash as hash

fn _bytes(str s) list {
   mut out = list(s.len)
   __list_set_len(out, s.len)
   mut i = 0
   while i < s.len {
      out[i] = load8(s, i)
      i += 1
   }
   out
}

def _ALPHABET = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"

fn crypt_decode6(str ch) int {
   "Returns the 6-bit value of a crypt alphabet character."
   mut i = 0
   while i < 64 {
      if str.str_slice(_ALPHABET, i, i + 1) == ch { return i }
      i += 1
   }
   return -1
}

fn crypt_encode6(int x) str {
   "Returns the crypt alphabet character for a 6-bit value, or an empty string."
   if x < 0 || x > 63 { return "" }
   str.str_slice(_ALPHABET, x, x + 1)
}

fn _b64(int value, int count) str {
   mut out = str.Builder(count)
   mut i = 0
   while i < count {
      out = str.builder_append(out, crypt_encode6(value & 63))
      value = value >> 6
      i += 1
   }
   def result = str.builder_to_str(out)
   str.builder_free(out)
   result
}

fn _join(list a, list b) list {
   mut out = []
   mut i = 0
   while i < a.len { out = out.append(a[i]) i += 1 }
   i = 0
   while i < b.len { out = out.append(b[i]) i += 1 }
   out
}

fn _repeat(list data, int count) list {
   mut out = []
   mut i = 0
   while i < count {
      mut j = 0
      while j < data.len { out = out.append(data[j]) j += 1 }
      i += 1
   }
   out
}

fn _repeat_prefix(list data, int n) list {
   mut out = []
   if data.len == 0 { return out }
   mut i = 0
   while i < n { out = out.append(data[i % data.len]) i += 1 }
   out
}

fn _digest(list data, int kind) list {
   if kind == 1 { return hash.md5_bytes(data) }
   if kind == 5 { return hash.sha256(data) }
   hash.sha512(data).unhex
}

fn _store_list(ptr dst, int off, list src) int {
   mut i = 0
   while i < src.len {
      store8(dst, src[i], off + i)
      i += 1
   }
   off + src.len
}

fn _salt(str setting, int kind) list {
   def magic = "$" + to_str(kind) + "$"
   if !str.startswith(setting, magic) { return ["", 0, false, false] }
   mut body = str.str_slice(setting, 3, setting.len)
   mut rounds = 5000
   mut explicit = false
   if kind != 1 && str.startswith(body, "rounds=") {
      mut j = 7
      mut value = 0
      if j >= body.len || load8(body, j) < 48 || load8(body, j) > 57 { return ["", 0, false, false] }
      while j < body.len && load8(body, j) >= 48 && load8(body, j) <= 57 {
         def digit = load8(body, j) - 48
         if value > 999999999 / 10 { value = 999999999 }
         else {
            value = value * 10 + digit
            if value > 999999999 { value = 999999999 }
         }
         j += 1
      }
      if j >= body.len || load8(body, j) != 36 { return ["", 0, false, false] }
      rounds = value
      if rounds < 1000 { rounds = 1000 }
      explicit = true
      body = str.str_slice(body, j + 1, body.len)
   }
   def max_len = kind == 1 ? 8 : 16
   mut n = 0
   while n < body.len && n < max_len && load8(body, n) != 36 { n += 1 }
   def salt = str.str_slice(body, 0, n)
   [salt, rounds, explicit, true]
}

fn _digest_b64(list digest, list order) str {
   mut out = str.Builder(90)
   mut i = 0
   while i < order.len {
      def t = order[i]
      mut value = 0
      mut n = 0
      if t.len == 3 {
         value = (digest[t[0]] << 16) | (digest[t[1]] << 8) | digest[t[2]]
         n = 4
      } elif t.len == 2 {
         value = (digest[t[0]] << 8) | digest[t[1]]
         n = 3
      } else {
         value = digest[t[0]]
         n = 2
      }
      out = str.builder_append(out, _b64(value, n))
      i += 1
   }
   def result = str.builder_to_str(out)
   str.builder_free(out)
   result
}

fn crypt_md5(str word, str setting) str {
   "Hashes `word` with MD5-crypt using the salt in `setting`."
   def p = _salt(setting, 1)
   if !p[3] { return "" }
   def s = p[0]
   def pass = _bytes(word)
   def sb = _bytes(s)
   def alternate = _digest(_join(_join(pass, sb), pass), 1)
   mut ctx = _join(_join(pass, _bytes("$1$")), sb)
   ctx = _join(ctx, _repeat_prefix(alternate, pass.len))
   mut n = pass.len
   while n > 0 {
      if (n & 1) == 1 { ctx = ctx.append(0) }
      else { ctx = ctx.append(load8(word, 0)) }
      n = n >> 1
   }
   mut digest = _digest(ctx, 1)
   def scratch = malloc(pass.len * 2 + sb.len + 64)
   mut i = 0
   while i < 1000 {
      mut n = 0
      if (i & 1) == 1 { n = _store_list(scratch, n, pass) }
      else { n = _store_list(scratch, n, digest) }
      if i % 3 != 0 { n = _store_list(scratch, n, sb) }
      if i % 7 != 0 { n = _store_list(scratch, n, pass) }
      if (i & 1) == 1 { n = _store_list(scratch, n, digest) }
      else { n = _store_list(scratch, n, pass) }
      digest = hash.digest_mem(1, scratch, n)
      i += 1
   }
   free(scratch)
   def order = [[0,6,12], [1,7,13], [2,8,14], [3,9,15], [4,10,5], [11]]
   "$1$" + s + "$" + _digest_b64(digest, order)
}

fn _sha_crypt(str word, str setting, int kind) str {
   def p = _salt(setting, kind)
   if !p[3] { return "" }
   def salt = p[0]
   def rounds = int(p[1])
   def pw = _bytes(word)
   def sb = _bytes(salt)
   def alternate = _digest(_join(_join(pw, sb), pw), kind)
   mut ctx = _join(pw, sb)
   ctx = _join(ctx, _repeat_prefix(alternate, pw.len))
   mut n = pw.len
   while n > 0 {
      if (n & 1) == 1 { ctx = _join(ctx, alternate) }
      else { ctx = _join(ctx, pw) }
      n = n >> 1
   }
   mut digest = _digest(ctx, kind)
   def P = _repeat_prefix(_digest(_repeat(pw, pw.len), kind), pw.len)
   def S = _repeat_prefix(_digest(_repeat(sb, 16 + digest[0]), kind), sb.len)
   def scratch = malloc(P.len * 2 + S.len + digest.len * 2 + 8)
   mut i = 0
   while i < rounds {
      mut n = 0
      if (i & 1) == 1 { n = _store_list(scratch, n, P) }
      else { n = _store_list(scratch, n, digest) }
      if i % 3 != 0 { n = _store_list(scratch, n, S) }
      if i % 7 != 0 { n = _store_list(scratch, n, P) }
      if (i & 1) == 1 { n = _store_list(scratch, n, digest) }
      else { n = _store_list(scratch, n, P) }
      digest = hash.digest_mem(kind, scratch, n)
      i += 1
   }
   free(scratch)
   mut order = []
   if kind == 5 {
      order = [[0,10,20],[21,1,11],[12,22,2],[3,13,23],[24,4,14],[15,25,5],[6,16,26],[27,7,17],[18,28,8],[9,19,29],[31,30]]
   } else {
      order = [[0,21,42],[22,43,1],[44,2,23],[3,24,45],[25,46,4],[47,5,26],[6,27,48],[28,49,7],[50,8,29],[9,30,51],[31,52,10],[53,11,32],[12,33,54],[34,55,13],[56,14,35],[15,36,57],[37,58,16],[59,17,38],[18,39,60],[40,61,19],[62,20,41],[63]]
   }
   def magic = "$" + to_str(kind) + "$"
   def prefix = p[2] ? magic + "rounds=" + to_str(rounds) + "$" : magic
   prefix + salt + "$" + _digest_b64(digest, order)
}

fn crypt_sha256(str word, str setting) str { _sha_crypt(word, setting, 5) }

fn crypt_sha512(str word, str setting) str { _sha_crypt(word, setting, 6) }
def _CDES_IP = [
   58,50,42,34,26,18,10,2,60,52,44,36,28,20,12,4,
   62,54,46,38,30,22,14,6,64,56,48,40,32,24,16,8,
   57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,
   61,53,45,37,29,21,13,5,63,55,47,39,31,23,15,7
]

def _CDES_FP = [
   40,8,48,16,56,24,64,32,39,7,47,15,55,23,63,31,
   38,6,46,14,54,22,62,30,37,5,45,13,53,21,61,29,
   36,4,44,12,52,20,60,28,35,3,43,11,51,19,59,27,
   34,2,42,10,50,18,58,26,33,1,41,9,49,17,57,25
]

def _CDES_E = [
   32,1,2,3,4,5,4,5,6,7,8,9,8,9,10,11,12,13,
   12,13,14,15,16,17,16,17,18,19,20,21,20,21,22,23,24,25,
   24,25,26,27,28,29,28,29,30,31,32,1
]

def _CDES_P = [
   16,7,20,21,29,12,28,17,1,15,23,26,5,18,31,10,
   2,8,24,14,32,27,3,9,19,13,30,6,22,11,4,25
]

def _CDES_PC1 = [
   57,49,41,33,25,17,9,1,58,50,42,34,26,18,
   10,2,59,51,43,35,27,19,11,3,60,52,44,36,
   63,55,47,39,31,23,15,7,62,54,46,38,30,22,
   14,6,61,53,45,37,29,21,13,5,28,20,12,4
]

def _CDES_PC2 = [
   14,17,11,24,1,5,3,28,15,6,21,10,
   23,19,12,4,26,8,16,7,27,20,13,2,
   41,52,31,37,47,55,30,40,51,45,33,48,
   44,49,39,56,34,53,46,42,50,36,29,32
]

def _CDES_SHIFTS = [1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1]
def _CDES_S = [[ 14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7, 0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8, 4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0, 15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13 ], [ 15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10, 3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5, 0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15, 13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9 ], [ 10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8, 13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1, 13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7, 1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12 ], [ 7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15, 13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9, 10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4, 3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14 ], [ 2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9, 14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6, 4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14, 11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3 ], [ 12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11, 10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8, 9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6, 4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13 ], [ 4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1, 13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6, 1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2, 6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12 ], [ 13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7, 1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2, 7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8, 2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11 ]]

fn _des_permute(int x, int width, list table) int {
   mut y = 0
   mut i = 0
   while i < table.len {
      y = (y << 1) | ((x >> (width - table[i])) & 1)
      i += 1
   }
   y
}

fn _des_bytebit(list bytes, int bit) int {
   (bytes[(bit - 1) / 8] >> (7 - ((bit - 1) % 8))) & 1
}

fn _des_key(list key) list {
   mut bits = 0
   mut i = 0
   while i < _CDES_PC1.len {
      bits = (bits << 1) | _des_bytebit(key, _CDES_PC1[i])
      i += 1
   }
   mut c = bits >> 28
   mut d = bits & 0x0fffffff
   mut keys = []
   i = 0
   while i < 16 {
      def step = _CDES_SHIFTS[i]
      c = ((c << step) | (c >> (28 - step))) & 0x0fffffff
      d = ((d << step) | (d >> (28 - step))) & 0x0fffffff
      def subkey = _des_permute((c << 28) | d, 56, _CDES_PC2)
      keys = keys.append(subkey)
      i += 1
   }
   keys
}

fn _des_f(int right, int key, list expansion) int {
   def x = _des_permute(right, 32, expansion) ^ key
   mut s = 0
   mut box = 0
   while box < 8 {
      def chunk = (x >> (42 - box * 6)) & 63
      def row = ((chunk & 32) >> 4) | (chunk & 1)
      def column = (chunk >> 1) & 15
      s = (s << 4) | _CDES_S[box][row * 16 + column]
      box += 1
   }
   _des_permute(s, 32, _CDES_P)
}

fn _des_enc(list block, list keys, list expansion) list {
   mut l = 0
   mut r = 0
   mut i = 0
   while i < 32 {
      l = (l << 1) | _des_bytebit(block, _CDES_IP[i])
      r = (r << 1) | _des_bytebit(block, _CDES_IP[i + 32])
      i += 1
   }
   i = 0
   while i < 16 {
      def t = (l ^ _des_f(r, keys[i], expansion)) & 0xffffffff
      l = r
      r = t
      i += 1
   }
   mut out = []
   i = 0
   while i < 8 {
      mut byte = 0
      mut j = 0
      while j < 8 {
         def bit = _CDES_FP[i * 8 + j]
         byte = (byte << 1) | (bit <= 32 ? ((r >> (32 - bit)) & 1) : ((l >> (64 - bit)) & 1))
         j += 1
      }
      out = out.append(byte)
      i += 1
   }
   out
}

fn crypt_des(str word, str setting) str {
   "Hashes `word` with traditional DES crypt using the salt in `setting`."
   if setting.len < 2 { return "" }
   def salt = str.str_slice(setting, 0, 2)
   def a = crypt_decode6(str.str_slice(salt, 0, 1))
   def b = crypt_decode6(str.str_slice(salt, 1, 2))
   if a < 0 || b < 0 { return "" }
   def salt_bits = a | (b << 6)
   mut expansion = []
   mut ei = 0
   while ei < _CDES_E.len { expansion = expansion.append(_CDES_E[ei]) ei += 1 }
   mut i = 0
   while i < 12 {
      if ((salt_bits >> i) & 1) != 0 {
         def v = expansion[i]
         expansion[i] = expansion[i + 24]
         expansion[i + 24] = v
      }
      i += 1
   }
   mut key = [0,0,0,0,0,0,0,0]
   i = 0
   while i < 8 && i < word.len {
      key[i] = (load8(word, i) & 127) << 1
      i += 1
   }
   def keys = _des_key(key)
   mut block = [0,0,0,0,0,0,0,0]
   i = 0
   while i < 25 {
      block = _des_enc(block, keys, expansion)
      i += 1
   }
   mut out = str.Builder(13)
   out = str.builder_append(out, salt)
   i = 0
   while i < 11 {
      mut v = 0
      mut j = 0
      while j < 6 {
         def pos = 6 * i + j
         v = (v << 1) | (pos < 64 ? ((block[pos / 8] >> (7 - (pos % 8))) & 1) : 0)
         j += 1
      }
      out = str.builder_append(out, crypt_encode6(v))
      i += 1
   }
   def result = str.builder_to_str(out)
   str.builder_free(out)
   result
}

fn crypt_hash(str word, str setting) str {
   "Hashes `word` with the scheme named by the prefix of `setting`."
   if str.startswith(setting, "$1$") { return crypt_md5(word, setting) }
   if str.startswith(setting, "$5$") { return crypt_sha256(word, setting) }
   if str.startswith(setting, "$6$") { return crypt_sha512(word, setting) }
   if setting.len > 0 && load8(setting, 0) == 36 { return "" }
   crypt_des(word, setting)
}
