;; Keywords: encoding ascii bytes hex base16 base32 base45 base58 base64 base85 base91 base92 base65536 radix pem der asn1 xor uu rot13 math crypto
;; Reference:
;; - https://www.rfc-editor.org/rfc/rfc4648
;; References:
;; - std.math.crypto
module std.math.crypto.encoding(ascii, base, bytes, encoding, xor, uu, radix, scream, pairing, lowercase, uppercase, letters, digits, hexdigits, octdigits, punctuation, whitespace, printable, encode64, decode64, encode64_url, decode64_url, encode32, decode32, encode32_hex, decode32_hex, encode16, decode16, bytes_set, bytes_get, asn1_parse, asn1_decode_length, asn1_to_json, asn1_get_integer, asn1_get_sequence, asn1_integers, pem_decode, hex_encode, hex_decode, base64_encode, base64_decode, base64_encode_str, base64_decode_str, base64_encode_bytes, base64_decode_custom, base64_decode_nested, base64_decode_until_marker, base32_encode, base32_decode, base32_decode_custom, base32_decode_custom_lsb, a1z26_pairs_decode, ascii_integer, ascii_to_bin, bin_to_ascii, binary_to_text, bits_to_text_width, text_to_binary, octal_to_text, text_to_octal, hex_to_text, hex_to_byte_list, text_to_hex, rot13, rot_n, rot_alphabet, bits_to_bytes, bytes_to_bits, bytes_concat, bytes_concat3, bytes_repeat_value, byte_list_to_ascii, ascii_contains, extract_ascii_span, xor_with_repeating_key, base91_pair_decode, affine_bytes_decrypt, base45_decode, ascii85_decode, base92_decode, base65536_decode, base58_encode_bytes, base58_decode_str, base58check_encode_bytes, base58check_decode_str, digit_value, parse_radix_int, parse_octal_int, decimal_chunks_to_text, octal_chunks_to_text, keyed_alpha_decode, uu_decode_line, xor_with_single_byte, xor_bytes_hex, single_byte_xor_bruteforce, english_score, repeating_key_xor_keylength, repeating_key_xor_crack, multi_text_xor_keystream, hamming_distance, crib_drag, xor_two_ciphertexts, repeating_xor_key_from_prefix, scream_mark_decode, scream_mark_from_codepoint, scream_extract_marks, scream_decode_marks, scream_decode_text, cantor_pair, cantor_unpair, cantor_unpair_leaves)
use std.core
use std.math.bin
use std.math.nt
use std.math.crypto.encoding.xor as xor
use std.core.str
use std.math.crypto.encoding.base
use std.math.crypto.encoding.radix

fn _alphabet_index(str alphabet, str ch, int limit=0) int {
   def n = limit > 0 ? min(alphabet.len, limit) : alphabet.len
   mut i = 0
   while i < n {
      if utf8_slice(alphabet, i, i + 1, 1) == ch { return i }
      i += 1
   }
   -1
}

fn base64_encode_str(str s) str {
   "Encode a string to standard Base64."
   encode64(s)
}

fn base64_encode_bytes(list bytes) str {
   "Encode a byte list to standard Base64."
   bytes.base64
}

fn base64_decode_str(str s) str {
   "Decode a Base64 string to plain text."
   decode64(s)
}

fn _base64_translate_custom(str s, str alphabet) str {
   def std = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/="
   def alpha_n = alphabet.len
   def pad_ch = alpha_n >= 65 ? utf8_slice(alphabet, 64, 65, 1) : "="
   mut translated = ""
   mut i = 0
   while i < s.len {
      def ch = utf8_slice(s, i, i + 1, 1)
      if ch == pad_ch { translated = str_add(translated, "=") } else {
         def j = _alphabet_index(alphabet, ch, 64)
         translated = str_add(translated, j >= 0 ? utf8_slice(std, j, j + 1, 1) : ch)
      }
      i += 1
   }
   translated
}

fn base64_decode_custom(str s, str alphabet) list {
   "Decode Base64 text using a custom alphabet.
   A 64-character alphabet uses standard '=' padding ; a 65-character alphabet
   may provide its own padding character as element 65."
   _base64_translate_custom(s, alphabet).base64_decode
}

fn base64_decode_nested(str s, int layers) str {
   "Decode Base64 repeatedly for `layers` iterations.
   Returns the final plaintext string."
   mut cur = s
   mut i = 0
   while i < layers {
      cur = base64_decode_str(cur)
      i += 1
   }
   cur
}

fn _base64_char_ok(str ch) bool {
   if ch.len == 0 { return false }
   def c = ord(ch)
   ch == "=" || ch == "+" || ch == "/" || ch == "-" || ch == "_" || (c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122)
}

fn _base64_decode_candidate(str s) bool {
   if s.len == 0 || s.len % 4 != 0 { return false }
   mut i = 0
   while i < s.len {
      if !_base64_char_ok(utf8_slice(s, i, i + 1, 1)) { return false }
      i += 1
   }
   true
}

fn base64_decode_until_marker(str s, str marker="", int max_layers=64) str {
   "Decode Base64 while the input still looks Base64-shaped, or until `marker` appears."
   mut cur = s
   mut i = 0
   while i < max_layers && _base64_decode_candidate(cur) {
      cur = base64_decode_str(cur)
      if marker.len > 0 && str.find(cur, marker) >= 0 { return cur }
      i += 1
   }
   cur
}

fn base32_encode(str s) str {
   "Encode a string to standard Base32(RFC 4648)."
   encode32(s)
}

fn base32_decode(str s) str {
   "Decode a standard Base32(RFC 4648) string to plain text."
   decode32(s)
}

fn base32_decode_custom(str s, str alphabet) str {
   "Decode Base32 using a custom 32-character alphabet."
   mut clean = ""
   mut i = 0
   while i < s.len {
      def ch = utf8_slice(s, i, i + 1, 1)
      if ch != "=" { clean = str_add(clean, ch) }
      i += 1
   }
   mut bits = ""
   mut j = 0
   while j < clean.len {
      def ch = utf8_slice(clean, j, j + 1, 1)
      def idx = _alphabet_index(alphabet, ch)
      if idx >= 0 {
         mut k = 4
         while k >= 0 {
            bits = str_add(bits, to_str((idx >> k) & 1))
            k = k - 1
         }
      }
      j += 1
   }
   mut out = ""
   mut bi = 0
   while bi + 7 < bits.len {
      mut val = 0
      mut k = 0
      while k < 8 {
         val = val * 2 + atoi(utf8_slice(bits, bi + k, bi + k + 1, 1))
         k += 1
      }
      out = str_add(out, chr(val))
      bi = bi + 8
   }
   out
}

fn base32_decode_custom_lsb(str s, str alphabet) str {
   "Decode custom Base32 where each 5-bit symbol is appended little-endian to a rolling bit buffer."
   mut bitstream = 0
   mut bitlen = 0
   mut out = ""
   mut i = 0
   while i < s.len {
      def ch = utf8_slice(s, i, i + 1, 1)
      if ch != "=" {
         def val = _alphabet_index(alphabet, ch)
         if val >= 0 {
            bitstream = bitstream | (val << bitlen)
            bitlen += 5
            while bitlen >= 8 {
               out = str_add(out, chr(bitstream & 255))
               bitstream = bitstream >> 8
               bitlen -= 8
            }
         }
      }
      i += 1
   }
   out
}

fn a1z26_pairs_decode(str s, bool mirror=false, int shift=0) str {
   "Decode runs of two-digit A1Z26 values into uppercase letters.
   With mirror=true, values are first Atbash-mirrored as 1<->26 ; shift then rotates the 1..26 result."
   mut out = ""
   mut i = 0
   while i < s.len {
      def c = load8(s, i)
      if c >= 48 && c <= 57 {
         mut j = i
         while j < s.len && load8(s, j) >= 48 && load8(s, j) <= 57 { j += 1 }
         mut k = i
         while k + 1 < j {
            def n = atoi(utf8_slice(s, k, k + 2, 1))
            if n >= 1 && n <= 26 {
               mut v = mirror ? (27 - n) : n
               v = ((v + shift - 1) % 26 + 26) % 26 + 1
               out = str_add(out, chr(v + 64))
            } else {
               out = str_add(out, "?")
            }
            k += 2
         }
         i = j
      } else {
         out = str_add(out, chr(c))
         i += 1
      }
   }
   out
}

fn binary_to_text(str binary_str) str {
   "Convert a string of 0s and 1s(space-separated or 8-bit chunks) to ASCII text.
   Handles both '01000001' and '01000001 01000010' formats."
   def clean = str_replace(binary_str, " ", "")
   mut out = ""
   mut i = 0
   while i + 7 < clean.len {
      mut val = 0
      mut k = 0
      while k < 8 {
         def bit = utf8_slice(clean, i + k, i + k + 1, 1)
         val = val * 2 + (bit == "1" ? 1 : 0)
         k += 1
      }
      out = str_add(out, chr(val))
      i = i + 8
   }
   out
}

fn bits_to_text_width(any bits, int width=8) str {
   "Decode fixed-width MSB-first bit chunks to text.
   Use width 7 for seven-bit ASCII and width 8 for byte ASCII."
   if width <= 0 || width > 8 { panic("bits_to_text_width: width must be between 1 and 8") }
   mut clean = ""
   if is_str(bits) {
      clean = str_replace(str_replace(str_replace(str_replace(bits, " ", ""), "\t", ""), "\n", ""), "\r", "")
   } else {
      mut i = 0
      while i < bits.len {
         def bit = to_str(bits[i])
         if bit != "0" && bit != "1" { panic("bits_to_text_width: bits must contain only 0 or 1") }
         clean = str_add(clean, bit)
         i += 1
      }
   }
   if clean.len == 0 { return "" }
   if clean.len % width != 0 { panic("bits_to_text_width: number of bits must be a multiple of width") }
   mut out = ""
   mut i = 0
   while i < clean.len {
      mut val = 0
      mut k = 0
      while k < width {
         def bit = utf8_slice(clean, i + k, i + k + 1, 1)
         if bit != "0" && bit != "1" { panic("bits_to_text_width: bits must contain only 0 or 1") }
         val = val * 2 + (bit == "1" ? 1 : 0)
         k += 1
      }
      out = str_add(out, chr(val))
      i = i + width
   }
   out
}

fn ascii_integer(any bits) int {
   "Convert exactly 8 MSB-first bits to an ASCII integer."
   def n = bits.len
   if n != 8 { panic("ascii_integer: B must consist of 8 bits") }
   mut v = 0
   mut i = 0
   while i < 8 {
      mut b = 0
      if is_str(bits) {
         def raw = utf8_slice(bits, i, i + 1, 1)
         if raw != "0" && raw != "1" { panic("ascii_integer: bits must contain only 0 or 1") }
         b = atoi(raw)
      } else {
         b = int(__load_item(bits, i))
      }
      if b != 0 && b != 1 { panic("ascii_integer: bits must contain only 0 or 1") }
      v = (v << 1) | (b & 1)
      i += 1
   }
   v
}

fn ascii_to_bin(any value) str {
   "Encode ASCII text or a list of string chunks as a compact binary string."
   mut text = ""
   if is_list(value) {
      mut i = 0
      while i < value.len {
         if !is_str(value[i]) { panic("ascii_to_bin: list elements must be strings") }
         text = str_add(text, value[i])
         i += 1
      }
   } else {
      text = to_str(value)
   }
   def spaced = text_to_binary(text)
   str_replace(spaced, " ", "")
}

fn bin_to_ascii(any bits) str {
   "Decode a non-empty binary string/list whose length is a multiple of 8."
   if bits.len == 0 { panic("bin_to_ascii: B must be a non-empty binary string") }
   if bits.len % 8 != 0 { panic("bin_to_ascii: number of bits must be a multiple of 8") }
   mut out = ""
   mut i = 0
   while i < bits.len {
      mut block = []
      mut j = 0
      while j < 8 {
         block = block.append(is_str(bits) ? atoi(utf8_slice(bits, i + j, i + j + 1, 1)) : bits[i + j])
         j += 1
      }
      out = str_add(out, chr(ascii_integer(block)))
      i += 8
   }
   out
}

fn text_to_binary(str text) str {
   "Convert ASCII text to binary string(8-bit groups space-separated)."
   mut out = ""
   mut i = 0
   while i < text.len {
      def code = ord(utf8_slice(text, i, i + 1, 1))
      if i > 0 { out = str_add(out, " ") }
      mut bits = ""
      mut k = 7
      while k >= 0 {
         bits = str_add(bits, to_str((code >> k) & 1))
         k = k - 1
      }
      out = str_add(out, bits)
      i += 1
   }
   out
}

fn octal_to_text(str octal_str) str {
   "Decode space-separated octal values to ASCII text."
   def parts = split(octal_str, " ")
   mut out = ""
   mut i = 0
   while i < parts.len {
      def raw = parts.get(i)
      if raw.len > 0 {
         mut val = 0
         mut j = 0
         while j < raw.len {
            val = val * 8 + atoi(utf8_slice(raw, j, j + 1, 1))
            j += 1
         }
         if val > 0 && val < 256 { out = str_add(out, chr(val)) }
      }
      i += 1
   }
   out
}

fn text_to_octal(str text) str {
   "Convert ASCII text to space-separated octal values."
   mut out = ""
   mut i = 0
   while i < text.len {
      def code = ord(utf8_slice(text, i, i + 1, 1))
      if i > 0 { out = str_add(out, " ") }
      mut digits = ""
      mut v = code
      if v == 0 { digits = "0"
      } else {
         while v > 0 {
            digits = str_add(to_str(v % 8), digits)
            v = v / 8
         }
      }
      out = str_add(out, digits)
      i += 1
   }
   out
}

fn hex_to_text(str hex_str) str {
   "Decode a hex string(with or without spaces) to ASCII text."
   def clean = hex_normalize(hex_str)
   clean.unhex.text
}

fn hex_to_byte_list(str hex_str) list {
   "Decode a hex string(with or without spaces) to a list of byte values."
   def clean = hex_normalize(hex_str)
   clean.unhex
}

fn text_to_hex(str text) str {
   "Encode ASCII text to lowercase hex string."
   text.hex
}

fn rot_n(str text, int n) str {
   "Apply ROT-N shift to alphabetic characters only."
   mut out = ""
   mut i = 0
   while i < text.len {
      def code = ord(utf8_slice(text, i, i + 1, 1))
      if code >= 65 && code <= 90 { out = str_add(out, chr((code - 65 + n) % 26 + 65)) } elif code >= 97 && code <= 122 {
         out = str_add(out, chr((code - 97 + n) % 26 + 97))
      } else {
         out = str_add(out, utf8_slice(text, i, i + 1, 1))
      }
      i += 1
   }
   out
}

fn rot13(str text) str {
   "Apply ROT13 transformation to alphabetic characters."
   rot_n(text, 13)
}

fn rot_alphabet(str text, str alphabet, int shift) str {
   "Rotate characters found in a custom alphabet by `shift`, preserving other chars."
   mut out = ""
   def m = alphabet.len
   if m == 0 { return text }
   mut i = 0
   while i < text.len {
      def ch = utf8_slice(text, i, i + 1, 1)
      def pos = _alphabet_index(alphabet, ch)
      if pos >= 0 {
         def np = ((pos + shift) % m + m) % m
         out = str_add(out, utf8_slice(alphabet, np, np + 1, 1))
      } else {
         out = str_add(out, ch)
      }
      i += 1
   }
   out
}

fn bits_to_bytes(list bit_list) list {
   "Convert a list of 0/1 bits to a byte list(MSB first, padded to 8)."
   mut result = []
   mut i = 0
   while i + 7 < bit_list.len + 1 {
      mut val = 0
      mut k = 0
      while k < 8 && i + k < bit_list.len {
         val = val * 2 + bit_list[i + k]
         k += 1
      }
      while k < 8 {
         val = val * 2
         k += 1
      }
      result = result.append(val)
      i = i + 8
   }
   result
}

fn bytes_to_bits(list byte_list) list {
   "Convert a byte list to a list of 0/1 bits(MSB first)."
   mut result = []
   mut i = 0
   while i < byte_list.len {
      def b = byte_list[i]
      mut k = 7
      while k >= 0 {
         result = result.append((b >> k) & 1)
         k = k - 1
      }
      i += 1
   }
   result
}

fn bytes_concat(list a, list b) list {
   "Concatenate two byte lists."
   a.concat(b)
}

fn bytes_concat3(list a, list b, list c) list {
   "Concatenate three byte lists."
   a.concat(b).concat(c)
}

fn bytes_repeat_value(int value, int n) list {
   "Return a byte list containing value repeated n times."
   if n <= 0 { return list(0) }
   mut out = []
   mut i = 0
   while i < n {
      out = out.append(value & 255)
      i += 1
   }
   out
}

fn byte_list_to_ascii(list byte_list) str {
   "Convert a list of byte values to text by mapping each byte through `chr`."
   mut out = ""
   mut i = 0
   while i < byte_list.len {
      out = out + chr(byte_list.get(i) & 255)
      i += 1
   }
   out
}

fn _ascii_match_at(list byte_list, str pattern, int at) bool {
   mut j = 0
   while j < pattern.len {
      if byte_list[at + j] != ord(utf8_slice(pattern, j, j + 1, 1)) { return false }
      j += 1
   }
   true
}

fn ascii_contains(list byte_list, str needle) bool {
   "Return true if a byte list contains the ASCII substring `needle`."
   def bn, nn = byte_list.len, needle.len
   if bn < nn { return false }
   mut i = 0
   while i + nn <= bn {
      if _ascii_match_at(byte_list, needle, i) { return true }
      i += 1
   }
   false
}

fn extract_ascii_span(list byte_list, str prefix, str suffix) any {
   "Extract the first ASCII span that begins with `prefix` and ends with `suffix`."
   def bn, pn = byte_list.len, prefix.len
   def sn = suffix.len
   if pn == 0 || sn == 0 { return nil }
   mut i = 0
   while i + pn <= bn {
      if _ascii_match_at(byte_list, prefix, i) {
         mut k = i + pn
         while k + sn <= bn {
            if _ascii_match_at(byte_list, suffix, k) {
               mut out = []
               mut t = i
               while t < k + sn {
                  out = out.append(byte_list[t])
                  t += 1
               }
               return byte_list_to_ascii(out)
            }
            k += 1
         }
      }
      i += 1
   }
   nil
}

fn xor_with_repeating_key(list byte_list, list key_bytes) list {
   "XOR a byte list with a repeating key byte list."
   xor.xor_with_repeating_key(byte_list, key_bytes)
}

fn affine_bytes_decrypt(list byte_list, int a, int b) list {
   "Affine decrypt over bytes: pt = a^{-1} * (ct - b) mod 256.
   Returns a byte list."
   def aa = mod(Z(a), Z(256))
   def bb = mod(Z(b), Z(256))
   def inva = inverse_mod(aa, Z(256))
   assert(inva != nil, "affine_bytes_decrypt: a not invertible mod 256")
   mut out = []
   mut i = 0
   def n = byte_list.len
   while i < n {
      def c, p = mod(Z(byte_list[i]), Z(256)), mod(inva * (c - bb), Z(256))
      out = out.append(bigint_to_int(p))
      i += 1
   }
   out
}

impl str {
   @inline
   fn b64(str s) str {
      "Return base64 encoding of this string."
      base64_encode_str(s)
   }
   @inline
   fn b64_text(str s) str {
      "Decode this base64 string to text."
      base64_decode_str(s)
   }
   @inline
   fn b64_bytes(str s) list {
      "Decode this base64 string to bytes."
      s.base64_decode
   }
   @inline
   fn b32(str s) str {
      "Return base32 encoding of this string."
      base32_encode(s)
   }
   @inline
   fn unb32(str s) str {
      "Decode this base32 string to text."
      base32_decode(s)
   }
   @inline
   fn b45_text(str s) str {
      "Decode this Base45 string to text."
      base45_decode(s)
   }
   @inline
   fn a85_text(str s) str {
      "Decode this Adobe Ascii85 string to text."
      ascii85_decode(s)
   }
   @inline
   fn b92_text(str s) str {
      "Decode this Base92 string to text."
      base92_decode(s)
   }
   @inline
   fn b65536_text(str s) str {
      "Decode this Base65536 string to text."
      base65536_decode(s)
   }
   @inline
   fn b58_bytes(str s) ?list {
      "Decode this base58 string to bytes."
      base58_decode_str(s)
   }
   @inline
   fn bin_text(str s) str {
      "Decode a binary digit string to text."
      binary_to_text(s)
   }
   @inline
   fn bin_text_width(str s, int width) str {
      "Decode fixed-width MSB-first bit chunks to text."
      bits_to_text_width(s, width)
   }
   @inline
   fn oct_text(str s) str {
      "Decode an octal digit string to text."
      octal_to_text(s)
   }
   @inline
   fn hex_text(str s) str {
      "Decode a hexadecimal string to text."
      hex_to_text(s)
   }
   @inline
   fn rot(str s, int n) str {
      "Apply ROT-n alphabetic rotation to this string."
      rot_n(s, n)
   }
   @inline
   fn rot13(str s) str {
      "Apply ROT13 alphabetic rotation to this string."
      rot13(s)
   }
}

impl list {
   @inline
   fn b64(list b) str {
      "Return base64 encoding of this byte list."
      base64_encode_bytes(b)
   }
   @inline
   fn b58(list b) str {
      "Return base58 encoding of this byte list."
      base58_encode_bytes(b)
   }
   @inline
   fn b58check(list b) str {
      "Return Base58Check encoding of this byte list."
      base58check_encode_bytes(b)
   }
   @inline
   fn bits(list b) list {
      "Expand this byte list into bits."
      bytes_to_bits(b)
   }
   @inline
   fn ascii(list b) str {
      "Interpret this byte list as ASCII text."
      byte_list_to_ascii(b)
   }
   @inline
   fn xor_key(list b, list key) list {
      "XOR this byte list with a repeating key."
      xor_with_repeating_key(b, key)
   }
   @inline
   fn affine_dec(list b, int a, int off) list {
      "Affine-decrypt this byte list modulo 256."
      affine_bytes_decrypt(b, a, off)
   }
}

impl bytes {
   @inline
   fn b64(bytes b) str {
      "Return base64 encoding of this byte buffer."
      base64_encode_bytes(b.to_list)
   }
   @inline
   fn b58(bytes b) str {
      "Return base58 encoding of this byte buffer."
      base58_encode_bytes(b.to_list)
   }
   @inline
   fn b58check(bytes b) str {
      "Return Base58Check encoding of this byte buffer."
      base58check_encode_bytes(b.to_list)
   }
   @inline
   fn bits(bytes b) list {
      "Expand this byte buffer into bits."
      bytes_to_bits(b.to_list)
   }
   @inline
   fn ascii(bytes b) str {
      "Interpret this byte buffer as ASCII text."
      byte_list_to_ascii(b.to_list)
   }
   @inline
   fn xor_key(bytes b, list key) list {
      "XOR this byte buffer with a repeating key."
      xor_with_repeating_key(b.to_list, key)
   }
   @inline
   fn affine_dec(bytes b, int a, int off) list {
      "Affine-decrypt this byte buffer modulo 256."
      affine_bytes_decrypt(b.to_list, a, off)
   }
}

#main {
   def b64 = base64_encode_str("Hello")
   def back = base64_decode_str(b64)
   assert(back == "Hello", "base64 round-trip")
   def b32 = base32_encode("Hello")
   assert(b32.len > 0, "base32 non-empty")
   def b32back = base32_decode(b32)
   assert(b32back == "Hello", "base32 round-trip")
   def nested = base64_decode_nested("SGVsbG8=", 1)
   assert(nested == "Hello", "nested base64 1 layer")
   def bin_str = text_to_binary("A")
   assert(bin_str == "01000001", "A = 01000001")
   def back2 = binary_to_text("01000001")
   assert(back2 == "A", "binary->text A")
   assert(ascii_integer("01000001") == 65, "ascii_integer string")
   assert(ascii_integer([0, 1, 0, 0, 0, 0, 1, 1]) == 67, "ascii_integer list")
   assert(ascii_to_bin(["A", "b"]) == "0100000101100010", "ascii_to_bin list chunks")
   assert(bin_to_ascii("0100000101100010") == "Ab", "bin_to_ascii string")
   assert(bin_to_ascii([0,1,0,0,0,0,0,1]) == "A", "bin_to_ascii list")
   def oct_text = octal_to_text("101 102")
   assert(oct_text == "AB", "octal to text")
   assert(rot13("Hello") == "Uryyb", "rot13 Hello")
   assert(rot13("Uryyb") == "Hello", "rot13 inverse")
   assert(rot_alphabet("Z9", "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 1) == "0A", "custom alphabet rot")
   def hx = text_to_hex("AB")
   assert(hx == "4142", "text to hex")
   def back3 = hex_to_text("4142")
   assert(back3 == "AB", "hex to text")
   assert(hex_to_text("0x41 42") == "AB", "hex to text with prefix/spaces")
   def hb = hex_to_byte_list("4142")
   assert(hb.get(0) == 65 && hb.get(1) == 66, "hex to byte list")
   def hb2 = hex_to_byte_list("0X41 42")
   assert(hb2.get(0) == 65 && hb2.get(1) == 66, "hex to byte list with uppercase prefix")
   def bts = bytes_to_bits([65])
   assert(bts.get(0) == 0, "65 MSB = 0")
   assert(bts.get(7) == 1, "65 LSB = 1")
   assert(byte_list_to_ascii([65, 66]) == "AB", "byte list to ascii")
   assert(ascii_contains([65, 66, 67], "BC"), "ascii contains")
   assert(
      extract_ascii_span([120, 66, 69, 71, 73, 78, 32, 75, 69, 89, 32, 69, 78, 68, 121], "BEGIN ", " END") ==
      "BEGIN KEY END",
      "extract ascii span"
   )
   assert(byte_list_to_ascii(xor_with_repeating_key([65, 66], [1])) == "@C", "xor repeating key")
   assert(base91_pair_decode("!!").get(0) == 0, "base91 pair decode !! -> 0")
   def pt0 = [0, 1, 2, 250, 255]
   mut ct0 = []
   mut i = 0
   while i < pt0.len {
      ct0 = ct0.append((7 * pt0.get(i) + 13) & 255)
      i += 1
   }
   assert(affine_bytes_decrypt(ct0, 7, 13) == pt0, "affine bytes decrypt")
   def raw = bytes(3)
   bytes_set(raw, 0, 65)
   bytes_set(raw, 1, 66)
   bytes_set(raw, 2, 67)
   assert(raw.b64 == [65, 66, 67].b64, "bytes b64 method")
   assert(raw.b58 == [65, 66, 67].b58, "bytes b58 method")
   assert(raw.b58check == [65, 66, 67].b58check, "bytes b58check method")
   assert(raw.bits == [65, 66, 67].bits, "bytes bits method")
   assert(raw.ascii == "ABC", "bytes ascii method")
   assert(raw.xor_key([1]).ascii == "@CB", "bytes xor_key method")
   print("✓ std.math.crypto.encoding.encoding self-test passed")
}
