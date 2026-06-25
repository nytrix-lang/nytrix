;; Keywords: encoding base math crypto
;; Encoding routines for base/radix encoding and decoding.
;; Reference:
;; References:
;; - std.math.crypto.encoding
;; - std.math.crypto
module std.math.crypto.encoding.base(encode64, decode64, encode64_url, decode64_url, encode32, decode32, encode32_hex, decode32_hex, encode16, decode16)
use std.core
use std.core.str as str

def _BASE16_ALPHABET = "0123456789ABCDEF"

fn _base_builder_take(list b) str {
   def out = str.builder_to_str(b)
   str.builder_free(b)
   out
}

fn _b64_unmap(int c, bool url=false) int {
   if url {
      return case c {
         65..90 -> c - 65
         97..122 -> c - 71
         48..57 -> c + 4
         45 -> 62
         95 -> 63
         _ -> -1
      }
   }
   return case c {
      65..90 -> c - 65
      97..122 -> c - 71
      48..57 -> c + 4
      43 -> 62
      47 -> 63
      _ -> -1
   }
}

fn _base_hex_unmap(int c) int {
   case c {
      48..57 -> c - 48
      65..70 -> c - 55
      97..102 -> c - 87
      _ -> -1
   }
}

fn _encode64_internal(str data, str alphabet, bool padding=true) str {
   if !is_str(data) { return "" }
   def n = data.len
   mut out = str.Builder(max(16, ((n + 2) / 3) * 4 + 8))
   mut i = 0
   while i < n {
      def b1 = load8(data, i) & 255
      def b2 = (i + 1 < n) ? (load8(data, i + 1) & 255) : 0
      def b3 = (i + 2 < n) ? (load8(data, i + 2) & 255) : 0
      out = str.builder_append(out, chr(load8(alphabet, b1 >> 2)))
      out = str.builder_append(out, chr(load8(alphabet, ((b1 & 3) << 4) | (b2 >> 4))))
      if i + 1 < n { out = str.builder_append(out, chr(load8(alphabet, ((b2 & 15) << 2) | (b3 >> 6)))) } elif padding { out = str.builder_append(out, "=") }
      if i + 2 < n { out = str.builder_append(out, chr(load8(alphabet, b3 & 63))) } elif padding { out = str.builder_append(out, "=") }
      i += 3
   }
   _base_builder_take(out)
}

fn _decode64_internal(str s, bool url=false) str {
   if !is_str(s) { return "" }
   def n = s.len
   mut out = malloc(n)
   if !out { return "" }
   mut p, i = 0, 0
   while i < n {
      def v1 = load8(s, i)
      if v1 == 0 { break }
      def c1 = _b64_unmap(v1, url)
      if c1 == -1 {
         if v1 == 61 { break }
         i += 1
         while i < n && _b64_unmap(load8(s, i), url) == -1 { i += 1 }
         continue
      }
      if i + 1 >= n { break }
      def c2 = _b64_unmap(load8(s, i + 1), url)
      if c2 == -1 { break }
      store8(out, (c1 << 2) | (c2 >> 4), p)
      p += 1
      if i + 2 < n {
         def v3 = load8(s, i + 2)
         if v3 != 61 {
            def c3 = _b64_unmap(v3, url)
            if c3 != -1 {
               store8(out, ((c2 & 15) << 4) | (c3 >> 2), p)
               p += 1
               if i + 3 < n {
                  def v4 = load8(s, i + 3)
                  if v4 != 61 {
                     def c4 = _b64_unmap(v4, url)
                     if c4 != -1 {
                        store8(out, ((c3 & 3) << 6) | c4, p)
                        p += 1
                     }
                  }
               }
            }
         }
      }
      i += 4
   }
   init_str(out, p)
   out
}

fn encode64(str data) str {
   "Encodes a byte string into Base64(RFC 4648 Section 4)."
   _encode64_internal(data, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/")
}

fn decode64(str s) str {
   "Decodes a Base64 string into bytes(RFC 4648 Section 4)."
   _decode64_internal(s, false)
}

fn encode64_url(str data) str {
   "Encodes a byte string into Base64 URL and Filename Safe(RFC 4648 Section 5)."
   _encode64_internal(data, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")
}

fn decode64_url(str s) str {
   "Decodes a Base64 URL and Filename Safe string into bytes(RFC 4648 Section 5)."
   _decode64_internal(s, true)
}

fn _b32_unmap(int c, bool hex=false) int {
   if hex {
      return case c {
         48..57 -> c - 48
         65..86 -> c - 55
         97..118 -> c - 87
         _ -> -1
      }
   }
   return case c {
      65..90 -> c - 65
      97..122 -> c - 97
      50..55 -> c - 24
      _ -> -1
   }
}

fn _encode32_internal(str data, str alphabet, bool padding=true) str {
   if !is_str(data) { return "" }
   def n = data.len
   mut out = str.Builder(max(16, ((n + 4) / 5) * 8 + 8))
   mut i = 0
   while i < n {
      def b1 = load8(data, i) & 255
      def b2 = (i + 1 < n) ? (load8(data, i + 1) & 255) : 0
      def b3 = (i + 2 < n) ? (load8(data, i + 2) & 255) : 0
      def b4 = (i + 3 < n) ? (load8(data, i + 3) & 255) : 0
      def b5 = (i + 4 < n) ? (load8(data, i + 4) & 255) : 0
      out = str.builder_append(out, chr(load8(alphabet, b1 >> 3)))
      out = str.builder_append(out, chr(load8(alphabet, ((b1 & 7) << 2) | (b2 >> 6))))
      if i + 1 < n {
         out = str.builder_append(out, chr(load8(alphabet, (b2 >> 1) & 31)))
         out = str.builder_append(out, chr(load8(alphabet, ((b2 & 1) << 4) | (b3 >> 4))))
      } elif padding {
         out = str.builder_append(out, "======")
         break
      }
      if i + 2 < n { out = str.builder_append(out, chr(load8(alphabet, ((b3 & 15) << 1) | (b4 >> 7)))) } elif padding {
         out = str.builder_append(out, "====")
         break
      }
      if i + 3 < n {
         out = str.builder_append(out, chr(load8(alphabet, (b4 >> 2) & 31)))
         out = str.builder_append(out, chr(load8(alphabet, ((b4 & 3) << 3) | (b5 >> 5))))
      } elif padding {
         out = str.builder_append(out, "===")
         break
      }
      if i + 4 < n { out = str.builder_append(out, chr(load8(alphabet, b5 & 31))) } elif padding {
         out = str.builder_append(out, "=")
         break
      }
      i += 5
   }
   _base_builder_take(out)
}

fn _decode32_internal(str s, bool hex=false) str {
   if !is_str(s) { return "" }
   def n = s.len
   mut out = malloc(n)
   if !out { return "" }
   mut p = 0
   mut bits = 0
   mut val = 0
   mut i = 0
   while i < n {
      def c = load8(s, i)
      if c == 61 { break }
      def v = _b32_unmap(c, hex)
      if v == -1 {
         i += 1
         continue
      }
      val = (val << 5) | v
      bits += 5
      if bits >= 8 {
         store8(out, (val >> (bits - 8)) & 255, p)
         p += 1
         bits -= 8
      }
      i += 1
   }
   init_str(out, p)
   out
}

fn encode32(str data) str {
   "Encodes a byte string into Base32(RFC 4648 Section 6)."
   _encode32_internal(data, "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567")
}

fn decode32(str s) str {
   "Decodes a Base32 string into bytes(RFC 4648 Section 6)."
   _decode32_internal(s, false)
}

fn encode32_hex(str data) str {
   "Encodes a byte string into Base32 with Hex Alphabet(RFC 4648 Section 7)."
   _encode32_internal(data, "0123456789ABCDEFGHIJKLMNOPQRSTUV")
}

fn decode32_hex(str s) str {
   "Decodes a Base32 Hex string into bytes(RFC 4648 Section 7)."
   _decode32_internal(s, true)
}

fn encode16(str data) str {
   "Encodes a byte string into Base16(Hex) (RFC 4648 Section 8)."
   if !is_str(data) { return "" }
   def n = data.len
   mut out = str.Builder(max(16, n * 2 + 8))
   mut i = 0
   while i < n {
      def b = load8(data, i) & 255
      out = str.builder_append(out, chr(load8(_BASE16_ALPHABET, b >> 4)))
      out = str.builder_append(out, chr(load8(_BASE16_ALPHABET, b & 15)))
      i += 1
   }
   _base_builder_take(out)
}

fn decode16(str s) str {
   "Decodes a Base16(Hex) string into bytes(RFC 4648 Section 8)."
   if !is_str(s) { return "" }
   def n = s.len
   mut out = malloc(n / 2)
   if !out { return "" }
   mut p, i = 0, 0
   while i + 1 < n {
      def c1, c2 = load8(s, i), load8(s, i + 1)
      def v1, v2 = _base_hex_unmap(c1), _base_hex_unmap(c2)
      if v1 != -1 && v2 != -1 {
         store8(out, (v1 << 4) | v2, p)
         p += 1
      }
      i += 2
   }
   init_str(out, p)
   out
}

#main {
   def s = "hello"
   def enc64 = encode64(s)
   assert(enc64 == "aGVsbG8=", "base64 encode")
   assert(decode64(enc64) == s, "base64 decode")
   def enc32 = encode32(s)
   assert(enc32 == "NBSWY3DP", "base32 encode")
   assert(decode32(enc32) == s, "base32 decode")
   def enc32hex = encode32_hex(s)
   assert(enc32hex == "D1IMOR3F", "base32 hex encode")
   assert(decode32_hex(enc32hex) == s, "base32 hex decode")
   assert(encode32_hex("") == "", "base32 hex vector 1")
   assert(encode32_hex("f") == "CO======", "base32 hex vector 2")
   assert(encode32_hex("fo") == "CPNG====", "base32 hex vector 3")
   assert(encode32_hex("foo") == "CPNMU===", "base32 hex vector 4")
   assert(encode32_hex("foob") == "CPNMUOG=", "base32 hex vector 5")
   assert(encode32_hex("fooba") == "CPNMUOJ1", "base32 hex vector 6")
   assert(encode32_hex("foobar") == "CPNMUOJ1E8======", "base32 hex vector 7")
   assert(decode32_hex("") == "", "base32 hex decode vector 1")
   assert(decode32_hex("CO======") == "f", "base32 hex decode vector 2")
   assert(decode32_hex("CPNG====") == "fo", "base32 hex decode vector 3")
   assert(decode32_hex("CPNMU===") == "foo", "base32 hex decode vector 4")
   assert(decode32_hex("CPNMUOG=") == "foob", "base32 hex decode vector 5")
   assert(decode32_hex("CPNMUOJ1") == "fooba", "base32 hex decode vector 6")
   assert(decode32_hex("CPNMUOJ1E8======") == "foobar", "base32 hex decode vector 7")
   def enc16 = encode16(s)
   assert(enc16 == "68656C6C6F", "base16 encode")
   assert(decode16(enc16) == s, "base16 decode")
   assert(decode16("68656c6c6f") == s, "base16 decode lowercase")
   assert(encode64_url("\xff\xff\xff") == "____", "base64 url encode")
   assert(decode64_url("____") == "\xff\xff\xff", "base64 url decode")
   assert(encode16("ABC") == "414243", "base16 encode uppercase hex")
   assert(decode16("414243") == "ABC", "base16 decode uppercase hex")
   assert(decode16("616263") == "abc", "base16 decode lowercase hex")
   assert(decode16("41zz42") == "AB", "base16 skips invalid pairs")
   print("✓ std.math.crypto.encoding.base self-test passed")
}
