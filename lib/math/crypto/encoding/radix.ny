;; Keywords: encoding radix math crypto
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc4648
;; - https://cacr.uwaterloo.ca/hac/about/chap1.pdf
;; References:
;; - std.math.crypto.encoding
;; - std.math.crypto
module std.math.crypto.encoding.radix(digit_value, parse_radix_int, parse_octal_int,
   decimal_chunks_to_text, octal_chunks_to_text, keyed_alpha_decode,
   base_n_encode_int, base_n_decode_int, base_n_encode_bytes, base_n_decode_bytes,
   base36_encode_int, base36_decode_int, base62_encode_int, base62_decode_int,
   base62_encode_bytes, base62_decode_bytes, base45_decode, ascii85_decode,
   base92_decode, base65536_decode, base58_encode_bytes, base58_decode_str,
   base58check_encode_bytes, base58check_decode_str, base91_pair_decode)
use std.core
use std.core.str
use std.math.nt
use std.math.crypto.hash as h

def BASE58_CHARS = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
def BASE36_CHARS = "0123456789abcdefghijklmnopqrstuvwxyz"
def BASE62_CHARS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
mut _BASE91_DEFAULT = nil

fn digit_value(int ch) int {
   "Return numeric digit value for ASCII code ch, or -1 if invalid."
   case ch {
      48..57 -> ch - 48
      97..122 -> 10 + ch - 97
      65..90 -> 10 + ch - 65
      _ -> -1
   }
}

fn parse_radix_int(any s, int base) int {
   "Parse a base-2..36 integer string, returning -1 on invalid input."
   if !is_str(s) || base < 2 || base > 36 { return -1 }
   mut out = 0
   mut i = 0
   while i < s.len {
      def d = digit_value(load8(s, i))
      if d < 0 || d >= base { return -1 }
      out = out * base + d
      i += 1
   }
   return out
}

fn parse_octal_int(any s) int {
   "Parse an octal integer string, returning -1 on invalid input."
   return parse_radix_int(s, 8)
}

fn _base_n_index(str alphabet, int ch) int {
   mut i = 0
   while i < alphabet.len {
      if load8(alphabet, i) == ch { return i }
      i += 1
   }
   -1
}

fn _base_n_valid_alphabet(str alphabet) bool {
   if !is_str(alphabet) || alphabet.len < 2 { return false }
   mut i = 0
   while i < alphabet.len {
      if _base_n_index(alphabet, load8(alphabet, i)) != i { return false }
      i += 1
   }
   true
}

fn base_n_encode_int(any n, str alphabet) str {
   "Encode a non-negative integer with an arbitrary unique alphabet."
   assert(_base_n_valid_alphabet(alphabet), "base_n_encode_int: alphabet must contain at least two unique bytes")
   mut x = Z(n)
   assert(x >= Z(0), "base_n_encode_int: negative integers are not supported")
   if x == Z(0) { return chr(load8(alphabet, 0)) }
   def base = Z(alphabet.len)
   mut digits = []
   while x > Z(0) {
      def q, r = x / base, x - q * base
      digits = digits.append(bigint_to_int(r))
      x = q
   }
   mut out = Builder(max(8, digits.len + 4))
   mut i = digits.len
   while i > 0 {
      i -= 1
      out = builder_append_byte(out, load8(alphabet, digits[i]))
   }
   _radix_finish_builder(out)
}

fn base_n_decode_int(str s, str alphabet) any {
   "Decode an arbitrary-alphabet integer. Returns -1 on invalid input."
   if !_base_n_valid_alphabet(alphabet) || !is_str(s) || s.len == 0 { return Z(-1) }
   def base = Z(alphabet.len)
   mut n, i = Z(0), 0
   while i < s.len {
      def d = _base_n_index(alphabet, load8(s, i))
      if d < 0 { return Z(-1) }
      n = n * base + Z(d)
      i += 1
   }
   n
}

fn base_n_encode_bytes(list bytes, str alphabet) str {
   "Encode a byte list with an arbitrary unique alphabet, preserving leading zero bytes."
   if bytes == nil || bytes.len == 0 { return "" }
   assert(_base_n_valid_alphabet(alphabet), "base_n_encode_bytes: alphabet must contain at least two unique bytes")
   mut out = Builder(max(8, bytes.len * 2 + 4))
   mut z = 0
   while z < bytes.len && bytes[z] == 0 {
      out = builder_append_byte(out, load8(alphabet, 0))
      z += 1
   }
   def n = bytes_to_bigint(bytes)
   if n != Z(0) {
      def enc = base_n_encode_int(n, alphabet)
      out = builder_append(out, enc)
   }
   _radix_finish_builder(out)
}

fn base_n_decode_bytes(str s, str alphabet) ?list {
   "Decode arbitrary-alphabet text into a byte list, preserving leading zero digits. Returns nil on invalid input."
   if !_base_n_valid_alphabet(alphabet) || !is_str(s) { return nil }
   if s.len == 0 { return [] }
   mut z = 0
   while z < s.len && load8(s, z) == load8(alphabet, 0) { z += 1 }
   def n = base_n_decode_int(s, alphabet)
   if n < Z(0) { return nil }
   mut raw = []
   if n != Z(0) { raw = bigint_to_bytes(n) }
   mut out = list(z + raw.len)
   __list_set_len(out, z + raw.len)
   mut i = 0
   while i < z {
      __store_item_fast(out, i, 0)
      i += 1
   }
   i = 0
   while i < raw.len {
      __store_item_fast(out, z + i, raw[i])
      i += 1
   }
   out
}

fn base36_encode_int(any n) str {
   "Encode a non-negative integer as base36 using 0-9a-z."
   base_n_encode_int(n, BASE36_CHARS)
}

fn base36_decode_int(str s) any {
   "Decode a base36 integer using 0-9a-z. Returns -1 on invalid input."
   base_n_decode_int(s, BASE36_CHARS)
}

fn base62_encode_int(any n) str {
   "Encode a non-negative integer as base62 using 0-9A-Za-z."
   base_n_encode_int(n, BASE62_CHARS)
}

fn base62_decode_int(str s) any {
   "Decode a base62 integer using 0-9A-Za-z. Returns -1 on invalid input."
   base_n_decode_int(s, BASE62_CHARS)
}

fn base62_encode_bytes(list bytes) str {
   "Encode a byte list as base62 using 0-9A-Za-z."
   base_n_encode_bytes(bytes, BASE62_CHARS)
}

fn base62_decode_bytes(str s) ?list {
   "Decode base62 text into a byte list. Returns nil on invalid input."
   base_n_decode_bytes(s, BASE62_CHARS)
}

@inline
fn _radix_is_ws(int c) bool {
   return case c {
      9, 10, 13, 32 -> true
      _ -> false
   }
}

fn _radix_chunks_to_text(str text, int base) str {
   mut out = Builder(max(8, text.len / 2 + 4))
   mut value = 0
   mut valid = true
   mut seen = false
   mut i = 0
   while i <= text.len {
      def c = i < text.len ? load8(text, i) : 32
      if _radix_is_ws(c) {
         if seen && valid && value >= 0 { out = builder_append_byte(out, value & 255) }
         value = 0
         valid = true
         seen = false
      } else {
         def d = digit_value(c)
         if d < 0 || d >= base { valid = false }
         else {
            value = value * base + d
            seen = true
         }
      }
      i += 1
   }
   return _radix_finish_builder(out)
}

fn decimal_chunks_to_text(str text) str {
   "Decode space-separated decimal byte values into text."
   _radix_chunks_to_text(text, 10)
}

fn octal_chunks_to_text(str text) str {
   "Decode space-separated octal byte values into text."
   _radix_chunks_to_text(text, 8)
}

fn _radix_clean_linebreaks(str s) str {
   str_replace(str_replace(str_replace(s, "\n", ""), "\r", ""), "\t", "")
}

fn _radix_clean_whitespace(str s) str {
   str_replace(str_replace(str_replace(str_replace(s, " ", ""), "\t", ""), "\n", ""), "\r", "")
}

fn _base45_value(int c) int {
   return case c {
      48..57 -> c - 48
      65..90 -> 10 + c - 65
      32 -> 36
      36 -> 37
      37 -> 38
      42 -> 39
      43 -> 40
      45 -> 41
      46 -> 42
      47 -> 43
      58 -> 44
      _ -> -1
   }
}

fn base45_decode(str s) str {
   "Decode Base45 text to bytes interpreted as a string.
   Line breaks and tabs are ignored ; spaces are preserved because Base45 uses
   space as a valid alphabet symbol."
   def clean = _radix_clean_linebreaks(s)
   def n = clean.len
   mut out = Builder(max(8, (n * 2) / 3 + 4))
   mut i = 0
   while i < n {
      def c0 = _base45_value(load8(clean, i))
      assert(c0 >= 0, "base45_decode: invalid character")
      if i + 2 < n {
         def c1, c2 = _base45_value(load8(clean, i + 1)), _base45_value(load8(clean, i + 2))
         assert(c1 >= 0 && c2 >= 0, "base45_decode: invalid character")
         def v = c0 + c1 * 45 + c2 * 45 * 45
         assert(v >= 0 && v <= 65535, "base45_decode: triplet out of byte-pair range")
         out = builder_append_byte(out, (v >> 8) & 255)
         out = builder_append_byte(out, v & 255)
         i += 3
      } else {
         assert(i + 1 < n, "base45_decode: dangling single character")
         def c1 = _base45_value(load8(clean, i + 1))
         assert(c1 >= 0, "base45_decode: invalid character")
         def v = c0 + c1 * 45
         assert(v >= 0 && v <= 255, "base45_decode: pair out of byte range")
         out = builder_append_byte(out, v)
         i += 2
      }
   }
   _radix_finish_builder(out)
}

fn _ascii85_append_value(list out, int v, int take=4) list {
   assert(v >= 0, "ascii85_decode: negative value")
   mut b = out
   if take >= 1 { b = builder_append_byte(b, (v >> 24) & 255) }
   if take >= 2 { b = builder_append_byte(b, (v >> 16) & 255) }
   if take >= 3 { b = builder_append_byte(b, (v >> 8) & 255) }
   if take >= 4 { b = builder_append_byte(b, v & 255) }
   b
}

fn ascii85_decode(str s) str {
   "Decode Adobe Ascii85/Base85 text to bytes interpreted as a string.
   Whitespace is ignored and optional <~ ~> markers are accepted."
   mut clean = _radix_clean_whitespace(s)
   if clean.len >= 4 && utf8_slice(clean, 0, 2, 1) == "<~" && utf8_slice(clean, clean.len - 2, clean.len, 1) == "~>" {
      clean = utf8_slice(clean, 2, clean.len - 2, 1)
   }
   mut out = Builder(max(8, (clean.len * 4) / 5 + 4))
   mut value = 0
   mut count = 0
   mut i = 0
   while i < clean.len {
      def c = load8(clean, i)
      if c == 122 {
         assert(count == 0, "ascii85_decode: z inside a partial group")
         out = _ascii85_append_value(out, 0, 4)
      } else {
         assert(c >= 33 && c <= 117, "ascii85_decode: invalid character")
         value = value * 85 + c - 33
         count += 1
         if count == 5 {
            out = _ascii85_append_value(out, value, 4)
            value = 0
            count = 0
         }
      }
      i += 1
   }
   if count > 0 {
      assert(count >= 2, "ascii85_decode: dangling single character")
      def take = count - 1
      while count < 5 {
         value = value * 85 + 84
         count += 1
      }
      out = _ascii85_append_value(out, value, take)
   }
   _radix_finish_builder(out)
}

fn _base92_value(int c) int {
   return case c {
      33 -> 0
      35..95 -> c - 34
      97..125 -> c - 35
      _ -> -1
   }
}

fn _radix_finish_builder(list out) str {
   def text = builder_to_str(out)
   builder_free(out)
   text
}

fn base92_decode(str s) str {
   "Decode Base92 text to bytes interpreted as a string.
   ASCII whitespace is ignored ; `~` decodes to the empty byte string."
   def clean = _radix_clean_whitespace(s)
   if clean == "~" { return "" }
   assert(clean.len != 1, "base92_decode: one character is not valid")
   mut bits = 0
   mut bit_count = 0
   mut out = Builder(max(8, clean.len))
   mut i = 0
   while i < clean.len - 1 {
      def v0, v1 = _base92_value(load8(clean, i) & 255), _base92_value(load8(clean, i + 1) & 255)
      assert(v0 >= 0 && v1 >= 0, "base92_decode: invalid character")
      def chunk = v0 * 91 + v1
      assert(chunk < 8192, "base92_decode: invalid 13-bit chunk")
      bits = (bits << 13) | chunk
      bit_count += 13
      while bit_count >= 8 {
         out = builder_append_byte(out, (bits >> (bit_count - 8)) & 255)
         bits = bits & ((1 << (bit_count - 8)) - 1)
         bit_count -= 8
      }
      i += 2
   }
   if i < clean.len {
      def v = _base92_value(load8(clean, i) & 255)
      assert(v >= 0, "base92_decode: invalid character")
      bits = (bits << 6) | v
      bit_count += 6
      while bit_count >= 8 {
         out = builder_append_byte(out, (bits >> (bit_count - 8)) & 255)
         bits = bits & ((1 << (bit_count - 8)) - 1)
         bit_count -= 8
      }
   }
   _radix_finish_builder(out)
}

fn _base65536_lookup(int cp) list {
   mut z2 = -1
   if cp >= 0x3400 && cp <= 0x4cff {
      z2 = cp - 0x3400
   } elif cp >= 0x4e00 && cp <= 0x9eff {
      z2 = 6400 + cp - 0x4e00
   } elif cp >= 0xa100 && cp <= 0xa3ff {
      z2 = 27136 + cp - 0xa100
   } elif cp >= 0xa500 && cp <= 0xa5ff {
      z2 = 27904 + cp - 0xa500
   } elif cp >= 0x10600 && cp <= 0x106ff {
      z2 = 28160 + cp - 0x10600
   } elif cp >= 0x12000 && cp <= 0x122ff {
      z2 = 28416 + cp - 0x12000
   } elif cp >= 0x13000 && cp <= 0x133ff {
      z2 = 29184 + cp - 0x13000
   } elif cp >= 0x14400 && cp <= 0x145ff {
      z2 = 30208 + cp - 0x14400
   } elif cp >= 0x16800 && cp <= 0x169ff {
      z2 = 30720 + cp - 0x16800
   } elif cp >= 0x20000 && cp <= 0x285ff {
      z2 = 31232 + cp - 0x20000
   }
   if z2 >= 0 { return [16, ((z2 & 255) << 8) | (z2 >> 8)] }
   if cp >= 0x1500 && cp <= 0x15ff { return [8, cp - 0x1500] }
   []
}

fn base65536_decode(str s) str {
   "Decode qntm Base65536 text to bytes interpreted as a string.
   ASCII whitespace is ignored so wrapped payloads can be pasted directly."
   def clean = _radix_clean_whitespace(s)
   mut out = Builder(max(8, clean.len))
   mut bits = 0
   mut bit_count = 0
   mut stop = false
   mut i = 0
   def n = utf8_len(clean)
   while i < n {
      assert(!stop, "base65536_decode: secondary character after final byte")
      def item = _base65536_lookup(ord_at(clean, i))
      assert(item.len == 2, "base65536_decode: unrecognised character")
      def zbits = item[0]
      def z = item[1]
      bits = (bits << zbits) | z
      bit_count += zbits
      while bit_count >= 8 {
         out = builder_append_byte(out, (bits >> (bit_count - 8)) & 255)
         bits = bits & ((1 << (bit_count - 8)) - 1)
         bit_count -= 8
      }
      if zbits != 16 { stop = true }
      i += 1
   }
   assert(bit_count == 0, "base65536_decode: trailing partial byte")
   _radix_finish_builder(out)
}

fn keyed_alpha_decode(str nums_text, str key, int base=10) str {
   "Decode space-separated numbers shifted by a repeating alphabetic key."
   mut out = Builder(max(8, nums_text.len / 2 + 4))
   mut value = 0
   mut valid = true
   mut seen = false
   mut key_idx = 0
   mut i = 0
   while i <= nums_text.len {
      def c = i < nums_text.len ? load8(nums_text, i) : 32
      if _radix_is_ws(c) {
         if seen && valid && key.len > 0 {
            def k = load8(key, key_idx % key.len)
            def plain = ((value - k + 26) % 26) + 97
            out = builder_append_byte(out, plain)
            key_idx += 1
         }
         value = 0
         valid = true
         seen = false
      } else {
         def d = digit_value(c)
         if d < 0 || d >= base { valid = false }
         else {
            value = value * base + d
            seen = true
         }
      }
      i += 1
   }
   return _radix_finish_builder(out)
}

fn base58_encode_bytes(list bytes) str {
   "Encode bytes list as Base58(Bitcoin alphabet)."
   if bytes == nil { return "" }
   if bytes.len == 0 { return "" }
   mut nn = bytes.long
   def zero = Z(0)
   def z58 = Z(58)
   mut digits = list(0)
   while nn > zero {
      def q, r = nn / z58, nn - q * z58
      digits = digits.append(bigint_to_int(r))
      nn = q
   }
   mut out = Builder(max(8, bytes.len * 2 + 4))
   mut i = 0
   while i < bytes.len && bytes[i] == 0 {
      out = builder_append_byte(out, 49)
      i += 1
   }
   i = digits.len
   while i > 0 {
      i -= 1
      out = builder_append_byte(out, load8(BASE58_CHARS, digits[i]))
   }
   _radix_finish_builder(out)
}

fn _base58_value(int c) int {
   return case c {
      49..57 -> c - 49
      65..72 -> 9 + c - 65
      74..78 -> 17 + c - 74
      80..90 -> 22 + c - 80
      97..107 -> 33 + c - 97
      109..122 -> 44 + c - 109
      _ -> -1
   }
}

fn base58_decode_str(str s) ?list {
   "Decode a Base58 string(Bitcoin alphabet) into a bytes list.
   Returns nil on invalid characters."
   if !is_str(s) || s.len == 0 { return [] }
   mut z = 0
   while z < s.len && load8(s, z) == 49 { z += 1 }
   def z58 = Z(58)
   mut n, i = Z(0), 0
   while i < s.len {
      def idx = _base58_value(load8(s, i))
      if idx < 0 { return nil }
      n = n * z58 + Z(idx)
      i += 1
   }
   mut raw = []
   if n != Z(0) { raw = bigint_to_bytes(n) }
   mut out = list(z + raw.len)
   mut k = 0
   while k < z {
      out[k] = 0
      k += 1
   }
   i = 0
   while i < raw.len {
      out[z + i] = raw[i]
      i += 1
   }
   store64(out, z + raw.len, 0)
   out
}

fn base58check_encode_bytes(list payload_bytes) str {
   "Base58Check encode: Base58( payload || checksum4 ), checksum = SHA256(SHA256(payload))[:4]."
   def h1, h2 = h.sha256_bytes(payload_bytes), h.sha256_bytes(h1)
   def checksum = [h2[0], h2[1], h2[2], h2[3]]
   mut raw = clone(payload_bytes)
   raw = raw.extend(checksum)
   base58_encode_bytes(raw)
}

fn base58check_decode_str(str s) ?list {
   "Decode Base58Check string to payload bytes.
   Returns nil on parse failure or checksum mismatch."
   def raw = base58_decode_str(s)
   if raw == nil { return nil }
   def list raw_bytes = raw
   if raw_bytes.len < 4 { return nil }
   def payload = slice(raw_bytes, 0, raw_bytes.len - 4)
   def chk = slice(raw_bytes, raw_bytes.len - 4, raw_bytes.len)
   def h1 = h.sha256_bytes(payload)
   def h2 = h.sha256_bytes(h1)
   if chk.len != 4 { return nil }
   if chk.get(0) != h2.get(0) { return nil }
   if chk.get(1) != h2.get(1) { return nil }
   if chk.get(2) != h2.get(2) { return nil }
   if chk.get(3) != h2.get(3) { return nil }
   return payload
}

fn _base91_default_alphabet() str {
   if _BASE91_DEFAULT != nil { return _BASE91_DEFAULT }
   mut b, i = Builder(96), 33
   while i <= 123 {
      b = builder_append(b, chr(i))
      i += 1
   }
   def s = builder_to_str(b)
   builder_free(b)
   _BASE91_DEFAULT = s
   s
}

fn base91_pair_decode(str s, str alphabet="") list {
   "Decode a simple base-91 *pair* encoding used in some puzzle and legacy data formats.
   This is NOT basE91(bitpacking). It interprets input as 2-char digits over a
   91-char alphabet:
   byte = idx(c0)*91 + idx(c1)
   Returns a byte list."
   if alphabet.len == 0 { alphabet = _base91_default_alphabet() }
   assert(alphabet.len == 91, "base91_pair_decode: alphabet must be 91 chars")
   def n = s.len
   assert(n % 2 == 0, "base91_pair_decode: even length")
   mut idx = list(128)
   mut i = 0
   while i < 128 {
      idx[i] = -1
      i += 1
   }
   store64(idx, 128, 0)
   i = 0
   while i < 91 {
      def b = load8(alphabet, i)
      if b >= 0 && b < 128 { idx[b] = i }
      i += 1
   }
   mut out = list(n / 2)
   i = 0
   mut oi = 0
   while i < n {
      def b0, b1 = load8(s, i), load8(s, i + 1)
      def v0, v1 = (b0 >= 0 && b0 < 128) ? idx[b0] : -1, (b1 >= 0 && b1 < 128) ? idx[b1] : -1
      assert(v0 >= 0 && v1 >= 0, "base91_pair_decode: invalid digit")
      def v = v0 * 91 + v1
      assert(v >= 0 && v <= 255, "base91_pair_decode: digit out of byte range")
      out[oi] = v
      oi += 1
      i += 2
   }
   store64(out, oi, 0)
   out
}

#main {
   assert(digit_value(ord("7")) == 7, "digit value decimal")
   assert(digit_value(ord("B")) == 11, "digit value alpha")
   assert(parse_radix_int("152", 8) == 106, "parse radix")
   assert(keyed_alpha_decode("152 162", "nnj", 8) == "we", "keyed alpha")
   assert(base36_encode_int(35) == "z", "base36 max digit")
   assert(base36_encode_int(123456789) == "21i3v9", "base36 encode")
   assert(base36_decode_int("21i3v9") == 123456789, "base36 decode")
   assert(base36_decode_int("21I3V9") == Z(-1), "base36 strict alphabet")
   assert(base62_encode_int(61) == "z", "base62 max digit")
   assert(base62_encode_int(3843) == "zz", "base62 encode")
   assert(base62_decode_int("zz") == 3843, "base62 decode")
   assert(base62_decode_int("!") == Z(-1), "base62 invalid digit")
   def payload = [0, 0, 1, 2, 255]
   def encoded = base62_encode_bytes(payload)
   assert(encoded.len > 2, "base62 bytes encoded")
   assert(base62_decode_bytes(encoded) == payload, "base62 bytes round trip")
   assert(base_n_encode_int(255, "01") == "11111111", "base-n binary encode")
   assert(base_n_decode_int("11111111", "01") == 255, "base-n binary decode")
   print("✓ std.math.crypto.encoding.radix self-test passed")
}
