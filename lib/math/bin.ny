;; Keywords: bin binary bytes packing
;; Binary and byte operations for endian access, bit manipulation, packing, and padding.
;; Endian reads/writes, bit operations, packing, padding, and byte codecs.
module std.math.bin(u8, u16le, u16be, u32le, u32be, u64le, u64be, f32le,
   unpack_le32, unpack_be32, unpack_le64, unpack_be64,
   p16le, p16be, p32le, p32be, p64le, p64be,
   u16le_vec, u32le_vec, u64le_vec,
   rotl, rotr, ror, rol, _u32, _and32, _or32, _xor32, _not32,
   _add32, _lshr32, _rotl32, _load_le32,
   bit_reverse, byte_reverse, set_bit, get_bit, clear_bit, toggle_bit,
   bit_count, trailing_zeros, leading_zeros,
   bytes_to_long, long_to_bytes, bytes_to_hex, hex_to_bytes,
   hex_normalize, hex_is_valid, bytes_to_base64, base64_to_bytes,
   bytes_xor, bytes_concat, bytes_concat3, bytes_repeat,
   bytes_reverse, bytes_trim_leading_zeros,
   str_to_bytes, bytes_to_list, bytes_to_str, zero_list, zero_bytes, from_list,
   pkcs7_pad, pkcs7_unpad, zero_pad, zero_unpad, bit_pad, bit_unpad,
   swap16, swap32, swap64, to_le16, to_le32, to_le64, to_be16, to_be32, to_be64,
   extract_bits, insert_bits, mask_bits, expand_bits, compress_bits,
bigint_to_bin_fixed)

use std.core
use std.core.error
use std.core.str
use std.math.crypto.encoding.base
use std.math.big
use std.math.simmd as simmd

fn _bin_max_int(int: a, int: b): int { a > b ? a : b }

@inline
fn _byte_list_new(int: n): list<int> {
   mut out = list(n)
   __list_set_len(out, n)
   out
}

@inline
fn _byte_list_store(list: out, int: i, int: value): any { __store_item_fast(out, i, value) }

@inline
fn _bytes_like_len(any: x): int {
   if(is_str(x) || is_bytes(x)){ return load64(x, -16) }
   if(is_list(x)){ return x.len }
   0
}

@inline
fn _hex_nibble(int: c): int {
   case c {
      48..57 -> c - 48
      65..70 -> c - 55
      97..102 -> c - 87
      _ -> 0
   }
}

@inline
fn _hex_is_digit(int: c): bool { (c >= 48 && c <= 57) || (c >= 65 && c <= 70) || (c >= 97 && c <= 102) }

fn _read_u8(any: s, int: i=0): int {
   load8(s, i) & 255
}

fn u8(any: s, int: i=0): int {
   "Read unsigned 8-bit value from byte string s at offset i."
   _read_u8(s, i)
}

fn u16le(any: s, int: i=0): int {
   "Read unsigned 16-bit little-endian value."
   (load8(s, i) | (load8(s, i + 1) << 8)) & 65535
}

fn u16be(any: s, int: i=0): int {
   "Read unsigned 16-bit big-endian value."
   ((load8(s, i) << 8) | load8(s, i + 1)) & 65535
}

fn u32le(any: s, int: i=0): int {
   "Read unsigned 32-bit little-endian value."
   (load8(s, i) | (load8(s, i + 1) << 8) |
   (load8(s, i + 2) << 16) | (load8(s, i + 3) << 24)) & 4294967295
}

fn u32be(any: s, int: i=0): int {
   "Read unsigned 32-bit big-endian value."
   ((load8(s, i) << 24) | (load8(s, i + 1) << 16) |
   (load8(s, i + 2) << 8) | load8(s, i + 3)) & 4294967295
}

fn u64le(any: s, int: i=0): int {
   "Read unsigned 64-bit little-endian value."
   mut lo = u32le(s, i)
   mut hi = u32le(s, i + 4)
   lo | (hi << 32)
}

fn u64be(any: s, int: i=0): int {
   "Read unsigned 64-bit big-endian value."
   mut hi = u32be(s, i)
   mut lo = u32be(s, i + 4)
   (hi << 32) | lo
}

fn unpack_le32(list: b, int: i=0): int {
   "Read 32-bit little-endian value from bytes list."
   (b[i] | (b[i + 1] << 8) |
   (b[i + 2] << 16) | (b[i + 3] << 24)) & 4294967295
}

fn unpack_be32(list: b, int: i=0): int {
   "Read 32-bit big-endian value from bytes list."
   ((b[i] << 24) | (b[i + 1] << 16) |
   (b[i + 2] << 8) | b[i + 3]) & 4294967295
}

fn unpack_le64(list: b, int: i=0): int {
   "Read 64-bit little-endian value from bytes list."
   mut lo = unpack_le32(b, i)
   mut hi = unpack_le32(b, i + 4)
   lo | (hi << 32)
}

fn unpack_be64(list: b, int: i=0): int {
   "Read 64-bit big-endian value from bytes list."
   mut hi = unpack_be32(b, i)
   mut lo = unpack_be32(b, i + 4)
   (hi << 32) | lo
}

fn bigint_to_bin_fixed(any: x, int: width): str {
   "Render x as a fixed-width binary string of 0/1(most significant bit first)."
   def bx = bigint(x)
   def one = bigint_from_int(1)
   def two = bigint_from_int(2)
   mut out = Builder(_bin_max_int(16, width + 8))
   mut i = width - 1
   while(i >= 0){
      def bit = bigint_mod(bigint_div(bx, bigint_lshift(one, i)), two)
      out = builder_append(out, bigint_eq(bit, one) ? "1" : "0")
      i -= 1
   }
   def text = builder_to_str(out)
   builder_free(out)
   text
}

fn p16le(int: n): str {
   "Pack 16-bit value as little-endian bytes."
   mut out = malloc(2)
   if(!out){ return "" }
   store8(out, n & 255, 0)
   store8(out, (n >> 8) & 255, 1)
   init_str(out, 2)
}

fn p16be(int: n): str {
   "Pack 16-bit value as big-endian bytes."
   mut out = malloc(2)
   if(!out){ return "" }
   store8(out, (n >> 8) & 255, 0)
   store8(out, n & 255, 1)
   init_str(out, 2)
}

fn p32le(int: n): str {
   "Pack 32-bit value as little-endian bytes."
   mut out = malloc(4)
   if(!out){ return "" }
   store8(out, n & 255, 0)
   store8(out, (n >> 8) & 255, 1)
   store8(out, (n >> 16) & 255, 2)
   store8(out, (n >> 24) & 255, 3)
   init_str(out, 4)
}

fn p32be(int: n): str {
   "Pack 32-bit value as big-endian bytes."
   mut out = malloc(4)
   if(!out){ return "" }
   store8(out, (n >> 24) & 255, 0)
   store8(out, (n >> 16) & 255, 1)
   store8(out, (n >> 8) & 255, 2)
   store8(out, n & 255, 3)
   init_str(out, 4)
}

fn p64le(int: n): str {
   "Pack 64-bit value as little-endian bytes."
   mut out = malloc(8)
   if(!out){ return "" }
   mut i = 0
   while(i < 8){
      store8(out, (n >> (i * 8)) & 255, i)
      i += 1
   }
   init_str(out, 8)
}

fn p64be(int: n): str {
   "Pack 64-bit value as big-endian bytes."
   mut out = malloc(8)
   if(!out){ return "" }
   mut i = 0
   while(i < 8){
      store8(out, (n >> ((7 - i) * 8)) & 255, i)
      i += 1
   }
   init_str(out, 8)
}

fn u16le_vec(any: s): list {
   "Read all 16-bit LE values from string."
   mut result = list(0)
   mut n = _bytes_like_len(s)
   mut i = 0
   while(i + 1 < n){
      result = result.append(u16le(s, i))
      i += 2
   }
   result
}

fn u32le_vec(any: s): list {
   "Read all 32-bit LE values from string."
   mut result = list(0)
   mut n = _bytes_like_len(s)
   mut i = 0
   while(i + 3 < n){
      result = result.append(u32le(s, i))
      i += 4
   }
   result
}

fn u64le_vec(any: s): list {
   "Read all 64-bit LE values from string."
   mut result = list(0)
   mut n = _bytes_like_len(s)
   mut i = 0
   while(i + 7 < n){
      result = result.append(u64le(s, i))
      i += 8
   }
   result
}

fn rotl(int: x, int: n, int: bits=32): int {
   "Rotate left: rotate x left by n bits within bits-width."
   case bits {
      32 -> simmd.rotl32(x, n)
      64 -> simmd.rotl64(x, n)
      _ -> {
         def mask = (1 << bits) - 1
         def shift = n % bits
         ((x << shift) | (x >> (bits - shift))) & mask
      }
   }
}

fn rotr(int: x, int: n, int: bits=32): int {
   "Rotate right: rotate x right by n bits within bits-width."
   case bits {
      32 -> simmd.rotr32(x, n)
      64 -> simmd.rotr64(x, n)
      _ -> {
         def mask = (1 << bits) - 1
         def shift = n % bits
         ((x >> shift) | (x << (bits - shift))) & mask
      }
   }
}

fn rol(int: x, int: n, int: bits=32): int { rotl(x, n, bits) }

fn ror(int: x, int: n, int: bits=32): int { rotr(x, n, bits) }

fn _pow2_32(int: n): int {
   mut v, i = 1, 0
   while(i < n){
      v = v * 2
      i += 1
   }
   v
}

fn _u32(int: x): int {
   x & 4294967295
}

fn _and32(int: a, int: b): int {
   (a & 4294967295) & (b & 4294967295)
}

fn _or32(int: a, int: b): int {
   (a & 4294967295) | (b & 4294967295)
}

fn _xor32(int: a, int: b): int {
   (a & 4294967295) ^^ (b & 4294967295)
}

fn _not32(int: x): int { 4294967295 - (x & 4294967295) }

fn _add32(int: a, int: b): int { (a + b) & 4294967295 }

fn _lshr32(int: x, int: n): int {
   if(n <= 0){ return _u32(x) }
   if(n >= 32){ return 0 }
   _u32(x) / _pow2_32(n)
}

fn _rotl32(int: x, int: n): int { simmd.rotl32(x, n) }

fn _load_le32(any: s, int: i): int { load8(s, i) + load8(s, i + 1) * 256 + load8(s, i + 2) * 65536 + load8(s, i + 3) * 16777216 }

fn bit_reverse(int: x, int: bits=32): int {
   "Reverse the order of bits in x."
   mut result = 0
   mut i = 0
   while(i < bits){
      if((x >> i) & 1){ result = result | (1 << (bits - 1 - i)) }
      i += 1
   }
   result
}

fn byte_reverse(int: x): int {
   "Reverse byte order(32-bit)."
   ((x & 0xFF) << 24) |
   ((x & 0xFF00) << 8) |
   ((x & 0xFF0000) >> 8) |
   ((x >> 24) & 0xFF)
}

fn set_bit(int: x, int: n): int {
   "Set bit n of x to 1."
   x | (1 << n)
}

fn get_bit(int: x, int: n): int {
   "Get bit n of x(returns 0 or 1)."
   (x >> n) & 1
}

fn clear_bit(int: x, int: n): int {
   "Clear bit n of x to 0."
   x & ~(1 << n)
}

fn toggle_bit(int: x, int: n): int {
   "Toggle bit n of x."
   x ^^ (1 << n)
}

fn bit_count(int: x): int {
   "Count number of set bits(population count)."
   if(x >= 0){ return simmd.popcnt64(x) }
   mut count = 0
   mut temp = x
   while(temp > 0){
      count += temp & 1
      temp = temp >> 1
   }
   count
}

fn trailing_zeros(int: x): int {
   "Count trailing zero bits."
   if(x == 0){ return 0 }
   if(x > 0){ return simmd.ctz64(x) }
   mut count = 0
   while((x & 1) == 0){
      count += 1
      x = x >> 1
   }
   count
}

fn leading_zeros(int: x, int: bits=32): int {
   "Count leading zero bits."
   if(x == 0){ return bits }
   if(bits == 32 && x > 0){ return simmd.clz32(x) }
   if(bits == 64 && x > 0){ return simmd.clz64(x) }
   mut count = 0
   mut i = bits - 1
   while(i >= 0 && ((x >> i) & 1) == 0){
      count += 1
      i -= 1
   }
   count
}

fn _bytes_to_long(list: b): bigint {
   "Convert bytes(list of ints) to long integer(big-endian)."
   def base = bigint_from_int(256)
   mut result = bigint_from_int(0)
   mut i = 0
   while(i < b.len){
      result = bigint_add(bigint_mul(result, base), bigint_from_int(b[i]))
      i += 1
   }
   result
}

fn _long_to_bytes(any: n, int: length=0): list<int> {
   "Convert long integer to bytes list(big-endian)."
   def zero = bigint_from_int(0)
   def base = bigint_from_int(256)
   if(bigint_eq(bigint(n), zero)){
      if(length > 0){
         mut z, i = list(0), 0
         while(i < length){
            z = z.append(0)
            i += 1
         }
         return z
      }
      return [0]
   }
   mut result = list(0)
   mut temp = bigint(n)
   while(bigint_gt(temp, zero)){
      result = result.append(bigint_to_int(bigint_mod(temp, base)))
      temp = bigint_div(temp, base)
   }
   mut i, j = 0, result.len - 1
   while(i < j){
      def tmp = result[i]
      result[i] = result[j]
      result[j] = tmp
      i += 1
      j -= 1
   }
   if(length > result.len){
      mut pad = length - result.len
      mut new_result = list(0)
      mut k = 0
      while(k < pad){
         new_result = new_result.append(0)
         k += 1
      }
      k = 0
      while(k < result.len){
         new_result = new_result.append(result[k])
         k += 1
      }
      result = new_result
   }
   result
}

fn _bytes_to_hex(list: b): str {
   "Convert bytes list to hex string."
   def n = b.len
   if(n <= 0){ return "" }
   def out_len = n * 2
   def out = malloc(out_len + 1)
   if(!out){ return "" }
   mut i, p = 0, 0
   while(i < n){
      def byte = b[i] & 255
      def hi = byte / 16
      def lo = byte % 16
      store8(out, hi < 10 ? (48 + hi) : (87 + hi), p)
      store8(out, lo < 10 ? (48 + lo) : (87 + lo), p + 1)
      i += 1
      p += 2
   }
   store8(out, 0, out_len)
   init_str(out, out_len)
}

fn _hex_to_bytes(str: hex_str): list<int> {
   "Convert hex string to bytes list."
   def n = _bytes_like_len(hex_str)
   if(n <= 0){ return list(0) }
   def out_n = (n + 1) >> 1
   mut result = _byte_list_new(out_n)
   mut out_i = 0
   mut i = 0
   if(n % 2 == 1){
      _byte_list_store(result, out_i, _hex_nibble(load8(hex_str, 0)))
      out_i += 1
      i = 1
   }
   while(i < n){
      def val1, val2 = _hex_nibble(load8(hex_str, i)), _hex_nibble(load8(hex_str, i + 1))
      _byte_list_store(result, out_i, val1 * 16 + val2)
      out_i += 1
      i += 2
   }
   result
}

fn hex_normalize(str: s): str {
   "Normalize hex text by stripping ASCII whitespace and `0x`/`0X` markers."
   def compact = str_replace(str_replace(str_replace(str_replace(s, " ", ""), "\t", ""), "\n", ""), "\r", "")
   str_replace(str_replace(compact, "0x", ""), "0X", "")
}

fn hex_is_valid(str: s, bool: even=true): bool {
   "Returns true when normalized hex text contains only hex digits.
   By default, also requires an even number of digits."
   s = hex_normalize(s)
   def n = _bytes_like_len(s)
   if(n <= 0){ return false }
   if(even && (n % 2) != 0){ return false }
   mut i = 0
   while(i < n){
      if(!_hex_is_digit(load8(s, i))){ return false }
      i += 1
   }
   true
}

fn _bytes_concat(list: a, list: b): list {
   "Concatenate two byte lists."
   def an, bn = a.len, b.len
   mut out = _byte_list_new(an + bn)
   mut i = 0
   while(i < an){
      _byte_list_store(out, i, a[i])
      i += 1
   }
   mut j = 0
   while(j < bn){
      _byte_list_store(out, an + j, b[j])
      j += 1
   }
   out
}

fn bytes_concat3(list: a, list: b, list: c): list {
   "Concatenate three byte lists."
   def an, bn = a.len, b.len
   def cn = c.len
   mut out = _byte_list_new(an + bn + cn)
   mut i = 0
   while(i < an){
      _byte_list_store(out, i, a[i])
      i += 1
   }
   mut j = 0
   while(j < bn){
      _byte_list_store(out, an + j, b[j])
      j += 1
   }
   mut k = 0
   while(k < cn){
      _byte_list_store(out, an + bn + k, c[k])
      k += 1
   }
   out
}

fn _bytes_xor(list: b1, list: b2): list {
   "XOR two byte lists."
   mut n1, n2 = b1.len, b2.len
   mut n = n1
   if(n2 < n){ n = n2 }
   mut result = _byte_list_new(n)
   mut i = 0
   while(i < n){
      _byte_list_store(result, i, bxor(b1[i], b2[i]))
      i += 1
   }
   result
}

fn _bytes_repeat(list: b, int: n): list {
   "Repeat byte list b, n times."
   if(n <= 0 || b.len <= 0){ return list(0) }
   def blen = b.len
   def out_n = blen * n
   mut result = _byte_list_new(out_n)
   mut i = 0
   mut pos = 0
   while(i < n){
      mut j = 0
      while(j < blen){
         _byte_list_store(result, pos, b[j])
         j += 1
         pos += 1
      }
      i += 1
   }
   result
}

fn _bytes_reverse(list: b): list {
   "Reverse byte list."
   def n = b.len
   mut result = _byte_list_new(n)
   mut i = 0
   while(i < n){
      _byte_list_store(result, i, b[n - 1 - i])
      i += 1
   }
   result
}

fn _bytes_trim_leading_zeros(list: b): list {
   "Remove leading zero bytes, preserving a single zero for an all-zero input."
   mut i = 0
   while(i + 1 < b.len && b[i] == 0){ i += 1 }
   slice(b, i, b.len)
}

@jit
fn f32le(any: s, int: i=0): f32 {
   "Reads a 32-bit float(IEEE 754) little-endian from byte string `s` at offset `i`."
   load32_f32(s, i)
}

fn _bytes_to_base64(list: b): str {
   "Encode bytes list to base64 string."
   def chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
   mut result = Builder(_bin_max_int(16, ((b.len + 2) / 3) * 4 + 8))
   mut i = 0
   while(i < b.len){
      def b1 = b[i]
      mut b2, b3 = 0, 0
      if(i + 1 < b.len){ b2 = b[i + 1] }
      if(i + 2 < b.len){ b3 = b[i + 2] }
      def c1, c2 = b1 >> 2, ((b1 & 3) << 4) | (b2 >> 4)
      result = builder_append(result, slice(chars, c1, c1 + 1))
      result = builder_append(result, slice(chars, c2, c2 + 1))
      if(i + 1 < b.len){
         def c3 = ((b2 & 15) << 2) | (b3 >> 6)
         result = builder_append(result, slice(chars, c3, c3 + 1))
      } else {
         result = builder_append(result, "=")
      }
      if(i + 2 < b.len){
         def c4 = b3 & 63
         result = builder_append(result, slice(chars, c4, c4 + 1))
      } else {
         result = builder_append(result, "=")
      }
      i += 3
   }
   def out = builder_to_str(result)
   builder_free(result)
   out
}

fn _base64_to_bytes(str: b64_str): list<int> {
   "Decode base64 string to bytes list."
   decode64(b64_str).to_bytes
}

fn _str_to_bytes(str: s): list<int> {
   "Convert string to bytes list."
   def n = _bytes_like_len(s)
   mut result = _byte_list_new(n)
   mut i = 0
   while(i < n){
      _byte_list_store(result, i, load8(s, i))
      i += 1
   }
   result
}

fn bytes_to_list(any: s): list<int> {
   "Convert bytes to a byte list."
   if(is_list(s)){ return s }
   def n = _bytes_like_len(s)
   mut result = _byte_list_new(n)
   mut i = 0
   while(i < n){
      _byte_list_store(result, i, load8(s, i))
      i += 1
   }
   result
}

fn _bytes_to_str(list: b): str {
   "Convert bytes list to string."
   def n = b.len
   if(n <= 0){ return "" }
   def out = malloc(n + 1)
   if(!out){ return "" }
   mut i = 0
   while(i < n){
      store8(out, b[i] & 255, i)
      i += 1
   }
   store8(out, 0, n)
   init_str(out, n)
}

impl list {
   @inline
   fn hex(list: b): str { _bytes_to_hex(b) }
   @inline
   fn base64(list: b): str { _bytes_to_base64(b) }
   @inline
   fn text(list: b): str { _bytes_to_str(b) }
   @inline
   fn le32(list: b, int: i=0): int { unpack_le32(b, i) }
   @inline
   fn be32(list: b, int: i=0): int { unpack_be32(b, i) }
   @inline
   fn le64(list: b, int: i=0): int { unpack_le64(b, i) }
   @inline
   fn be64(list: b, int: i=0): int { unpack_be64(b, i) }
   @inline
   fn xor(list: b, list: other): list { _bytes_xor(b, other) }
   @inline
   fn concat(list: b, list: other): list { _bytes_concat(b, other) }
   @inline
   fn repeat(list: b, int: n): list { _bytes_repeat(b, n) }
   @inline
   fn rev(list: b): list { _bytes_reverse(b) }
   @inline
   fn trim0(list: b): list { _bytes_trim_leading_zeros(b) }
}

impl bytes {
   @inline
   fn hex(bytes: b): str { _bytes_to_hex(bytes_to_list(b)) }
   @inline
   fn base64(bytes: b): str { _bytes_to_base64(bytes_to_list(b)) }
   @inline
   fn text(bytes: b): str { _bytes_to_str(bytes_to_list(b)) }
   @inline
   fn u8(bytes: b, int: i=0): int { _read_u8(b, i) }
   @inline
   fn le16(bytes: b, int: i=0): int { u16le(b, i) }
   @inline
   fn be16(bytes: b, int: i=0): int { u16be(b, i) }
   @inline
   fn le32(bytes: b, int: i=0): int { u32le(b, i) }
   @inline
   fn be32(bytes: b, int: i=0): int { u32be(b, i) }
   @inline
   fn le64(bytes: b, int: i=0): int { u64le(b, i) }
   @inline
   fn be64(bytes: b, int: i=0): int { u64be(b, i) }
}

impl str {
   @inline
   fn to_bytes(str: self): list<int> { _str_to_bytes(self) }
   @inline
   fn unhex(str: self): list<int> { _hex_to_bytes(self) }
   @inline
   fn long(str: self): bigint { _bytes_to_long(_str_to_bytes(self)) }
   @inline
   fn bytes(str: self): list<int> { _str_to_bytes(self) }
   @inline
   fn bytes_long(str: self): bigint { _bytes_to_long(_str_to_bytes(self)) }
   @inline
   fn base64_decode(str: s): list<int> { _base64_to_bytes(s) }
   @inline
   fn hex(str: s): str { _bytes_to_hex(_str_to_bytes(s)) }
   @inline
   fn base64(str: s): str { _bytes_to_base64(_str_to_bytes(s)) }
   @inline
   fn u8(str: s, int: i=0): int { _read_u8(s, i) }
   @inline
   fn le16(str: s, int: i=0): int { u16le(s, i) }
   @inline
   fn be16(str: s, int: i=0): int { u16be(s, i) }
   @inline
   fn le32(str: s, int: i=0): int { u32le(s, i) }
   @inline
   fn be32(str: s, int: i=0): int { u32be(s, i) }
   @inline
   fn le64(str: s, int: i=0): int { u64le(s, i) }
   @inline
   fn be64(str: s, int: i=0): int { u64be(s, i) }
}

impl int {
   @inline
   fn u32(int: x): int { _u32(x) }
   @inline
   fn rotl(int: x, int: n, int: bits=32): int { rotl(x, n, bits) }
   @inline
   fn rotr(int: x, int: n, int: bits=32): int { rotr(x, n, bits) }
   @inline
   fn rol(int: x, int: n, int: bits=32): int { rotl(x, n, bits) }
   @inline
   fn ror(int: x, int: n, int: bits=32): int { rotr(x, n, bits) }
   @inline
   fn bit(int: x, int: n): int { get_bit(x, n) }
   @inline
   fn set_bit(int: x, int: n): int { set_bit(x, n) }
   @inline
   fn clear_bit(int: x, int: n): int { clear_bit(x, n) }
   @inline
   fn toggle_bit(int: x, int: n): int { toggle_bit(x, n) }
   @inline
   fn bit_count(int: x): int { bit_count(x) }
   @inline
   fn trailing_zeros(int: x): int { trailing_zeros(x) }
   @inline
   fn leading_zeros(int: x, int: bits=32): int { leading_zeros(x, bits) }
   @inline
   fn swap16(int: x): int { swap16(x) }
   @inline
   fn swap32(int: x): int { swap32(x) }
   @inline
   fn swap64(int: x): int { swap64(x) }
   @inline
   fn extract_bits(int: x, int: start, int: width): int { extract_bits(x, start, width) }
   @inline
   fn insert_bits(int: x, int: value, int: start, int: width): int { insert_bits(x, value, start, width) }
   @inline
   fn mask_bits(int: x, int: start, int: width): int { mask_bits(x, start, width) }
   @inline
   fn bytes(int: x): list<int> { _long_to_bytes(x) }
}

fn bytes_to_long(list: b): bigint { b.long }

fn long_to_bytes(any: n, int: length=0): list<int> { _long_to_bytes(n, length) }

fn bytes_to_hex(list: b): str { b.hex }

fn hex_to_bytes(str: hex_str): list<int> { _hex_to_bytes(hex_str) }

fn bytes_to_base64(list: b): str { b.base64 }

fn base64_to_bytes(str: b64_str): list<int> { b64_str.base64_decode }

fn bytes_to_str(list: b): str { b.text }

fn str_to_bytes(str: s): list<int> { _str_to_bytes(s) }

fn bytes_concat(list: a, list: b): list { a.concat(b) }

fn bytes_xor(list: a, list: b): list { a.xor(b) }

fn bytes_repeat(list: b, int: n): list { b.repeat(n) }

fn bytes_reverse(list: b): list { b.rev }

fn bytes_trim_leading_zeros(list: b): list { b.trim0 }

fn zero_list(int: n): list {
   "Create list of n zeros."
   if(n <= 0){ return list(0) }
   mut out = _byte_list_new(n)
   mut i = 0
   while(i < n){
      _byte_list_store(out, i, 0)
      i += 1
   }
   out
}

fn zero_bytes(int: n): list {
   "Create a byte list of n zeros."
   zero_list(n)
}

fn from_list(list: xs): str {
   "Converts a list of byte values into a NUL-terminated byte string."
   def n = xs.len
   def out = malloc(n + 1)
   if(!out){ return "" }
   mut i = 0
   while(i < n){
      store8(out, xs[i] & 255, i)
      i += 1
   }
   store8(out, 0, n)
   init_str(out, n)
}

fn pkcs7_pad(list: data, int: block_size=16): list {
   "PKCS#7 padding for bytes list."
   mut padding_len = block_size - (data.len % block_size)
   mut result = clone(data)
   mut i = 0
   while(i < padding_len){
      result = result.append(padding_len)
      i += 1
   }
   result
}

fn pkcs7_unpad(list: data): list {
   "Remove PKCS#7 padding from bytes list."
   if(data.len == 0){ return data }
   def padding_len = data[data.len - 1]
   if(padding_len > data.len){ return data }
   mut result = list(0)
   mut i = 0
   while(i < data.len - padding_len){
      result = result.append(data[i])
      i += 1
   }
   result
}

fn zero_pad(list: data, int: block_size=16): list {
   "Zero padding for bytes list."
   mut padding_len = block_size - (data.len % block_size)
   if(padding_len == block_size){ padding_len = 0 }
   mut result = clone(data)
   mut i = 0
   while(i < padding_len){
      result = result.append(0)
      i += 1
   }
   result
}

fn zero_unpad(list: data): list {
   "Remove zero padding from bytes list."
   mut result = clone(data)
   while(result.len > 0 && result[result.len - 1] == 0){ result = slice(result, 0, result.len - 1) }
   result
}

fn bit_pad(list: data, int: block_bits=8): list {
   "Bit padding(1 followed by zeros)."
   mut result = clone(data)
   result = result.append(128) ;; 0x80 = 10000000
   while(result.len % (block_bits / 8) != 0){ result = result.append(0) }
   result
}

fn bit_unpad(list: data): list {
   "Remove bit padding."
   mut i = data.len - 1
   while(i >= 0 && data[i] == 0){ i -= 1 }
   if(i >= 0 && data[i] == 128){ return slice(data, 0, i) }
   data
}

fn swap16(int: n): int {
   "Swap bytes of 16-bit value."
   ((n & 0xFF) << 8) | ((n >> 8) & 0xFF)
}

fn swap32(int: n): int {
   "Swap bytes of 32-bit value."
   simmd.bswap32(n)
}

fn swap64(int: n): int {
   "Swap bytes of 64-bit value."
   simmd.bswap64(n)
}

fn to_le16(int: n): str { p16le(n) }

fn to_le32(int: n): str { p32le(n) }

fn to_le64(int: n): str { p64le(n) }

fn to_be16(int: n): str { p16be(n) }

fn to_be32(int: n): str { p32be(n) }

fn to_be64(int: n): str { p64be(n) }

fn extract_bits(int: x, int: start, int: width): int {
   "Extract width bits starting at position start."
   def mask = (1 << width) - 1
   (x >> start) & mask
}

fn insert_bits(int: x, int: value, int: start, int: width): int {
   "Insert value into x at position start with given width."
   def mask = ((1 << width) - 1) << start
   (x & ~mask) | ((value & ((1 << width) - 1)) << start)
}

fn mask_bits(int: x, int: start, int: width): int {
   "Create mask for bits from start to start+width."
   ((1 << width) - 1) << start
}

fn expand_bits(int: x, list: positions): int {
   "Expand bits of x to positions specified in list."
   if(positions.len <= 64){
      mut mask = 0
      mut ok = true
      mut p = 0
      while(p < positions.len){
         def pos = positions[p]
         if(!is_int(pos) || pos < 0 || pos >= 64){ ok = false }
         else { mask = mask | (1 << pos) }
         p += 1
      }
      if(ok){ return simmd.pdep64(x, mask) }
   }
   mut result = 0
   mut i = 0
   while(i < positions.len){
      if((x >> i) & 1){ result = result | (1 << positions[i]) }
      i += 1
   }
   result
}

fn compress_bits(int: x, list: positions): int {
   "Compress bits from positions into consecutive bits."
   if(positions.len <= 64){
      mut mask = 0
      mut ok = true
      mut p = 0
      while(p < positions.len){
         def pos = positions[p]
         if(!is_int(pos) || pos < 0 || pos >= 64){ ok = false }
         else { mask = mask | (1 << pos) }
         p += 1
      }
      if(ok){ return simmd.pext64(x, mask) }
   }
   mut result = 0
   mut out_pos = 0
   mut i = 0
   while(i < positions.len){
      if((x >> positions[i]) & 1){ result = result | (1 << out_pos) }
      out_pos += 1
      i += 1
   }
   result
}
