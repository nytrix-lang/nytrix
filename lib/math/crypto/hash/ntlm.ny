;; Keywords: hash ntlm
;; Hash-analysis routines for NTLM, LM, MD4, and DCC password hashes.
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc1320
module std.math.crypto.hash.ntlm(md4_bytes, md4_hex, lm_hash, ntlm_hash, dcc_hash, dcc2_hash)
use std.core
use std.math.bin (zero_list)
use std.math.crypto.symmetric.des
use std.core.str as str

fn _u32(int: x): int { x & 4294967295 }
fn _and32(int: a, int: b): int { (a & 4294967295) & (b & 4294967295) }
fn _or32(int: a, int: b): int { (a & 4294967295) | (b & 4294967295) }
fn _xor32(int: a, int: b): int { (a & 4294967295) ^^ (b & 4294967295) }
fn _not32(int: x): int { 4294967295 - (x & 4294967295) }
fn _add32(int: a, int: b): int { (a + b) & 4294967295 }
fn _rotl32(int: x, int: n): int {
   def k = n & 31
   if(k == 0){ return _u32(x) }
   _u32((x << k) | ((x & 4294967295) >> (32 - k)))
}

fn _be32_from_bytes(list: data, int: i): int {
   (__load_item_fast(data, i) << 24) | (__load_item_fast(data, i + 1) << 16) | (__load_item_fast(data, i + 2) << 8) | __load_item_fast(data, i + 3)
}

fn _sha1_bytes(list: data): list {
   def data_len = data.len
   def bit_len = data_len * 8
   def zero_len = (56 - ((data_len + 1) % 64) + 64) % 64
   def msg_len = data_len + 1 + zero_len + 8
   mut msg = zero_list(msg_len)
   mut i = 0
   while(i < data_len){
      __store_item_fast(msg, i, __load_item_fast(data, i))
      i += 1
   }
   __store_item_fast(msg, data_len, 0x80)
   mut k = 0
   while(k < 8){
      __store_item_fast(msg, msg_len - 8 + k, (bit_len / (1 << (8 * (7 - k)))) % 256)
      k += 1
   }
   mut h0, h1 = 0x67452301, 0xefcdab89
   mut h2, h3 = 0x98badcfe, 0x10325476
   mut h4 = 0xc3d2e1f0
   mut w = zero_list(80)
   mut off = 0
   while(off < msg.len){
      i = 0
      while(i < 16){
         __store_item_fast(w, i, _be32_from_bytes(msg, off + i * 4))
         i += 1
      }
      while(i < 80){
         __store_item_fast(w, i, _rotl32(_xor32(_xor32(__load_item_fast(w, i - 3), __load_item_fast(w, i - 8)), _xor32(__load_item_fast(w, i - 14), __load_item_fast(w, i - 16))), 1))
         i += 1
      }
      mut a, b = h0, h1
      mut c, d = h2, h3
      mut e = h4
      i = 0
      while(i < 80){
         mut f = 0
         mut kk = 0
         if(i < 20){
            f = _or32(_and32(b, c), _and32(_not32(b), d))
            kk = 0x5a827999
         } elif(i < 40){
            f = _xor32(_xor32(b, c), d)
            kk = 0x6ed9eba1
         } elif(i < 60){
            f = _or32(_or32(_and32(b, c), _and32(b, d)), _and32(c, d))
            kk = 0x8f1bbcdc
         } else {
            f = _xor32(_xor32(b, c), d)
            kk = 0xca62c1d6
         }
         def temp = _add32(_add32(_add32(_add32(_rotl32(a, 5), f), e), kk), __load_item_fast(w, i))
         e, d = d, c
         c, b = _rotl32(b, 30), a
         a = temp
         i += 1
      }
      h0, h1 = _add32(h0, a), _add32(h1, b)
      h2, h3 = _add32(h2, c), _add32(h3, d)
      h4 = _add32(h4, e)
      off += 64
   }
   mut out = zero_list(20)
   __store_item_fast(out, 0, (h0 >> 24) & 255)
   __store_item_fast(out, 1, (h0 >> 16) & 255)
   __store_item_fast(out, 2, (h0 >> 8) & 255)
   __store_item_fast(out, 3, h0 & 255)
   __store_item_fast(out, 4, (h1 >> 24) & 255)
   __store_item_fast(out, 5, (h1 >> 16) & 255)
   __store_item_fast(out, 6, (h1 >> 8) & 255)
   __store_item_fast(out, 7, h1 & 255)
   __store_item_fast(out, 8, (h2 >> 24) & 255)
   __store_item_fast(out, 9, (h2 >> 16) & 255)
   __store_item_fast(out, 10, (h2 >> 8) & 255)
   __store_item_fast(out, 11, h2 & 255)
   __store_item_fast(out, 12, (h3 >> 24) & 255)
   __store_item_fast(out, 13, (h3 >> 16) & 255)
   __store_item_fast(out, 14, (h3 >> 8) & 255)
   __store_item_fast(out, 15, h3 & 255)
   __store_item_fast(out, 16, (h4 >> 24) & 255)
   __store_item_fast(out, 17, (h4 >> 16) & 255)
   __store_item_fast(out, 18, (h4 >> 8) & 255)
   __store_item_fast(out, 19, h4 & 255)
   out
}

fn _hmac_sha1_with_pads(list: ipad, list: opad, list: message): list {
   mut inner = zero_list(64 + message.len)
   mut i = 0
   while(i < 64){
      __store_item_fast(inner, i, __load_item_fast(ipad, i))
      i += 1
   }
   i = 0
   while(i < message.len){
      __store_item_fast(inner, 64 + i, __load_item_fast(message, i))
      i += 1
   }
   def ih = _sha1_bytes(inner)
   mut outer = zero_list(84)
   i = 0
   while(i < 64){
      __store_item_fast(outer, i, __load_item_fast(opad, i))
      i += 1
   }
   i = 0
   while(i < ih.len){
      __store_item_fast(outer, 64 + i, __load_item_fast(ih, i))
      i += 1
   }
   _sha1_bytes(outer)
}

fn _hmac_sha1_pads(list: key): list {
   def src = (key.len > 64) ? _sha1_bytes(key) : key
   mut k = zero_list(64)
   mut i = 0
   while(i < src.len && i < 64){
      __store_item_fast(k, i, __load_item_fast(src, i))
      i += 1
   }
   mut ipad, opad = zero_list(64), zero_list(64)
   i = 0
   while(i < 64){
      def kb = __load_item_fast(k, i)
      __store_item_fast(ipad, i, kb ^^ 0x36)
      __store_item_fast(opad, i, kb ^^ 0x5c)
      i += 1
   }
   [ipad, opad]
}

fn _hmac_sha1_bytes(list: key, list: message): list {
   def pads = _hmac_sha1_pads(key)
   _hmac_sha1_with_pads(pads[0], pads[1], message)
}

fn _xor_bytes(list: a, list: b): list {
   mut out = zero_list(a.len)
   mut i = 0
   while(i < a.len){
      __store_item_fast(out, i, __load_item_fast(a, i) ^^ __load_item_fast(b, i))
      i += 1
   }
   out
}

fn _pbkdf2_hmac_sha1_16(list: password, list: salt, int: iterations): list {
   mut block_salt = zero_list(salt.len + 4)
   mut si = 0
   while(si < salt.len){
      __store_item_fast(block_salt, si, __load_item_fast(salt, si))
      si += 1
   }
   __store_item_fast(block_salt, salt.len + 3, 1)
   def pads = _hmac_sha1_pads(password)
   def ipad = pads[0]
   def opad = pads[1]
   mut u = _hmac_sha1_with_pads(ipad, opad, block_salt)
   mut acc = clone(u)
   mut i = 1
   while(i < iterations){
      u = _hmac_sha1_with_pads(ipad, opad, u)
      acc = _xor_bytes(acc, u)
      i += 1
   }
   mut out = zero_list(16)
   i = 0
   while(i < 16){
      __store_item_fast(out, i, __load_item_fast(acc, i))
      i += 1
   }
   out
}

fn _utf16le_bytes(str: s): list {
   def n = s.len * 2
   mut out = zero_list(n)
   mut i = 0
   while(i < s.len){
      __store_item_fast(out, i * 2, load8(s, i))
      i += 1
   }
   out
}

fn md4_bytes(list: data): list {
   "Return MD4 digest bytes for a byte list."
   def data_len = data.len
   def bit_len = data_len * 8
   def zero_len = (56 - ((data_len + 1) % 64) + 64) % 64
   def msg_len = data_len + 1 + zero_len + 8
   mut msg = zero_list(msg_len)
   mut i = 0
   while(i < data_len){
      __store_item_fast(msg, i, __load_item_fast(data, i))
      i += 1
   }
   __store_item_fast(msg, data_len, 0x80)
   mut k = 0
   while(k < 8){
      __store_item_fast(msg, msg_len - 8 + k, (bit_len / (1 << (8 * k))) % 256)
      k += 1
   }
   mut A, B = 0x67452301, 0xefcdab89
   mut C, D = 0x98badcfe, 0x10325476
   mut X = zero_list(16)
   mut off = 0
   while(off < msg.len){
      i = 0
      while(i < 16){
         def p = off + i * 4
         __store_item_fast(X, i, __load_item_fast(msg, p) | (__load_item_fast(msg, p + 1) << 8) | (__load_item_fast(msg, p + 2) << 16) | (__load_item_fast(msg, p + 3) << 24))
         i += 1
      }
      def AA = A
      def BB = B
      def CC = C
      def DD = D
      i = 0
      while(i < 16){
         def r = i % 4
         if(r == 0){
            def f = ((B & C) | ((4294967295 - (B & 4294967295)) & D)) & 4294967295
            A = _rotl32((A + f + __load_item_fast(X, i)) & 4294967295, 3)
         } elif(r == 1){
            def f = ((A & B) | ((4294967295 - (A & 4294967295)) & C)) & 4294967295
            D = _rotl32((D + f + __load_item_fast(X, i)) & 4294967295, 7)
         } elif(r == 2){
            def f = ((D & A) | ((4294967295 - (D & 4294967295)) & B)) & 4294967295
            C = _rotl32((C + f + __load_item_fast(X, i)) & 4294967295, 11)
         } else {
            def f = ((C & D) | ((4294967295 - (C & 4294967295)) & A)) & 4294967295
            B = _rotl32((B + f + __load_item_fast(X, i)) & 4294967295, 19)
         }
         i += 1
      }
      i = 0
      while(i < 16){
         def r = i % 4
         def xi = r * 4 + (i / 4)
         if(r == 0){
            def g = ((B & C) | (B & D) | (C & D)) & 4294967295
            A = _rotl32((A + g + __load_item_fast(X, xi) + 0x5a827999) & 4294967295, 3)
         } elif(r == 1){
            def g = ((A & B) | (A & C) | (B & C)) & 4294967295
            D = _rotl32((D + g + __load_item_fast(X, xi) + 0x5a827999) & 4294967295, 5)
         } elif(r == 2){
            def g = ((D & A) | (D & B) | (A & B)) & 4294967295
            C = _rotl32((C + g + __load_item_fast(X, xi) + 0x5a827999) & 4294967295, 9)
         } else {
            def g = ((C & D) | (C & A) | (D & A)) & 4294967295
            B = _rotl32((B + g + __load_item_fast(X, xi) + 0x5a827999) & 4294967295, 13)
         }
         i += 1
      }
      i = 0
      while(i < 16){
         def r = i % 4
         def g3 = i / 4
         mut xi = 0
         if(g3 == 0){
            if(r == 0){ xi = 0 } elif(r == 1){ xi = 8 } elif(r == 2){ xi = 4 } else { xi = 12 }
         } elif(g3 == 1){
            if(r == 0){ xi = 2 } elif(r == 1){ xi = 10 } elif(r == 2){ xi = 6 } else { xi = 14 }
         } elif(g3 == 2){
            if(r == 0){ xi = 1 } elif(r == 1){ xi = 9 } elif(r == 2){ xi = 5 } else { xi = 13 }
         } else {
            if(r == 0){ xi = 3 } elif(r == 1){ xi = 11 } elif(r == 2){ xi = 7 } else { xi = 15 }
         }
         if(r == 0){
            def h = (B ^^ C ^^ D) & 4294967295
            A = _rotl32((A + h + __load_item_fast(X, xi) + 0x6ed9eba1) & 4294967295, 3)
         } elif(r == 1){
            def h = (A ^^ B ^^ C) & 4294967295
            D = _rotl32((D + h + __load_item_fast(X, xi) + 0x6ed9eba1) & 4294967295, 9)
         } elif(r == 2){
            def h = (D ^^ A ^^ B) & 4294967295
            C = _rotl32((C + h + __load_item_fast(X, xi) + 0x6ed9eba1) & 4294967295, 11)
         } else {
            def h = (C ^^ D ^^ A) & 4294967295
            B = _rotl32((B + h + __load_item_fast(X, xi) + 0x6ed9eba1) & 4294967295, 15)
         }
         i += 1
      }
      A, B = (A + AA) & 4294967295, (B + BB) & 4294967295
      C, D = (C + CC) & 4294967295, (D + DD) & 4294967295
      off += 64
   }
   mut out = zero_list(16)
   __store_item_fast(out, 0, A & 255)
   __store_item_fast(out, 1, (A >> 8) & 255)
   __store_item_fast(out, 2, (A >> 16) & 255)
   __store_item_fast(out, 3, (A >> 24) & 255)
   __store_item_fast(out, 4, B & 255)
   __store_item_fast(out, 5, (B >> 8) & 255)
   __store_item_fast(out, 6, (B >> 16) & 255)
   __store_item_fast(out, 7, (B >> 24) & 255)
   __store_item_fast(out, 8, C & 255)
   __store_item_fast(out, 9, (C >> 8) & 255)
   __store_item_fast(out, 10, (C >> 16) & 255)
   __store_item_fast(out, 11, (C >> 24) & 255)
   __store_item_fast(out, 12, D & 255)
   __store_item_fast(out, 13, (D >> 8) & 255)
   __store_item_fast(out, 14, (D >> 16) & 255)
   __store_item_fast(out, 15, (D >> 24) & 255)
   out
}

fn md4_hex(list: data): str {
   "Return MD4 digest hex for a byte list."
   md4_bytes(data).hex
}

fn _set_odd_parity(int: b): int {
   def base = b & 0xfe
   mut ones = 0
   mut i = 1
   while(i < 8){
      ones += (base >> i) & 1
      i += 1
   }
   if((ones % 2) == 0){ return base | 1 }
   base
}

fn _lm_des_key_from_buf(list: buf, int: start): list {
   def b0, b1 = __load_item_fast(buf, start), __load_item_fast(buf, start + 1)
   def b2, b3 = __load_item_fast(buf, start + 2), __load_item_fast(buf, start + 3)
   def b4, b5 = __load_item_fast(buf, start + 4), __load_item_fast(buf, start + 5)
   def b6 = __load_item_fast(buf, start + 6)
   return [
      _set_odd_parity((b0 >> 0) & 0xfe),
      _set_odd_parity(((b0 << 7) | (b1 >> 1)) & 0xfe),
      _set_odd_parity(((b1 << 6) | (b2 >> 2)) & 0xfe),
      _set_odd_parity(((b2 << 5) | (b3 >> 3)) & 0xfe),
      _set_odd_parity(((b3 << 4) | (b4 >> 4)) & 0xfe),
      _set_odd_parity(((b4 << 3) | (b5 >> 5)) & 0xfe),
      _set_odd_parity(((b5 << 2) | (b6 >> 6)) & 0xfe),
      _set_odd_parity((b6 << 1) & 0xfe)
   ]
}

fn lm_hash(str: password): str {
   "Return the Windows LM hash of a password as lowercase hex.
   The password is uppercased, truncated/padded to 14 bytes, split into two
   7-byte DES keys, then used to encrypt the constant KGS!@#$%."
   mut upper_pw = str.upper(password)
   mut buf = zero_list(14)
   mut i = 0
   while(i < upper_pw.len && i < 14){
      __store_item_fast(buf, i, load8(upper_pw, i))
      i += 1
   }
   def magic = [75, 71, 83, 33, 64, 35, 36, 37]
   def k1 = _lm_des_key_from_buf(buf, 0)
   def k2 = _lm_des_key_from_buf(buf, 7)
   assert(is_list(k1) && k1.len == 8, "LM key half 1")
   assert(is_list(k2) && k2.len == 8, "LM key half 2")
   assert(is_list(magic) && magic.len == 8, "LM magic block")
   des_encrypt_block(k1, magic).concat(des_encrypt_block(k2, magic)).hex
}

fn ntlm_hash(str: password): str {
   "Return the NT hash, MD4(UTF-16LE(password)), as lowercase hex."
   md4_hex(_utf16le_bytes(password))
}

fn dcc_hash(str: password, str: username): str {
   "Return MSCache v1 / DCC hash: MD4(NT_hash_bytes || UTF-16LE(lower(username)))."
   def nt = md4_bytes(_utf16le_bytes(password))
   def user = _utf16le_bytes(str.lower(username))
   mut data = zero_list(nt.len + user.len)
   mut i = 0
   while(i < nt.len){
      __store_item_fast(data, i, __load_item_fast(nt, i))
      i += 1
   }
   i = 0
   while(i < user.len){
      __store_item_fast(data, nt.len + i, __load_item_fast(user, i))
      i += 1
   }
   md4_hex(data)
}

fn dcc2_hash(str: password, str: username, int: iterations=10240): str {
   "Return MSCache v2 / DCC2 hash: PBKDF2-HMAC-SHA1(DCC1, lower(username UTF-16LE), iterations)[:16]."
   def dcc1 = dcc_hash(password, username).unhex
   _pbkdf2_hmac_sha1_16(dcc1, _utf16le_bytes(str.lower(username)), iterations).hex
}

if(comptime{ return __main() }){
   assert(md4_hex([]) == "31d6cfe0d16ae931b73c59d7e0c089c0", "MD4 empty vector")
   assert(md4_hex("abc".to_bytes) == "a448017aaf21d8525fc10ae87aa6729d", "MD4 abc vector")
   assert(ntlm_hash("password") == "8846f7eaee8fb117ad06bdd830b7586c", "NTLM password vector")
}
