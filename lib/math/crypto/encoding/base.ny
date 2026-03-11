;; Keywords: encoding base
;; Encoding routines for base/radix encoding and decoding.
;; Reference:
module std.math.crypto.encoding.base(encode64, decode64, encode64_url, decode64_url, encode32, decode32, encode32_hex, decode32_hex, encode16, decode16)
use std.core
use std.core.str as str

fn _base_builder_take(list: b): str {
   def out = str.builder_to_str(b)
   str.builder_free(b)
   out
}

fn _b64_unmap(int: c, bool: url=false): int {
   if(url){
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

fn _base_hex_unmap(int: c): int {
   case c {
      48..57 -> c - 48
      65..70 -> c - 55
      97..102 -> c - 87
      _ -> -1
   }
}

fn _encode64_internal(str: data, str: alphabet, bool: padding=true): str {
   if(!is_str(data)){ return "" }
   def n = data.len
   mut out = str.Builder(max(16, ((n + 2) / 3) * 4 + 8))
   mut i = 0
   while(i < n){
      def b1 = load8(data, i) & 255
      def b2 = (i + 1 < n) ? (load8(data, i + 1) & 255) : 0
      def b3 = (i + 2 < n) ? (load8(data, i + 2) & 255) : 0
      out = str.builder_append(out, chr(load8(alphabet, b1 >> 2)))
      out = str.builder_append(out, chr(load8(alphabet, ((b1 & 3) << 4) | (b2 >> 4))))
      if(i + 1 < n){ out = str.builder_append(out, chr(load8(alphabet, ((b2 & 15) << 2) | (b3 >> 6)))) } elif(padding){ out = str.builder_append(out, "=") }
      if(i + 2 < n){ out = str.builder_append(out, chr(load8(alphabet, b3 & 63))) } elif(padding){ out = str.builder_append(out, "=") }
      i += 3
   }
   _base_builder_take(out)
}

fn _decode64_internal(str: s, bool: url=false): str {
   if(!is_str(s)){ return "" }
   def n = s.len
   mut out = malloc(n)
   if(!out){ return "" }
   mut p, i = 0, 0
   while(i < n){
      def v1 = load8(s, i)
      if(v1 == 0){ break }
      def c1 = _b64_unmap(v1, url)
      if(c1 == -1){
         if(v1 == 61){ break } ; RFC 4648 padding char '='
         i += 1
         ; Skip other characters
         while(i < n && _b64_unmap(load8(s, i), url) == -1){ i += 1 }
         continue
      }
      if(i + 1 >= n){ break }
      def c2 = _b64_unmap(load8(s, i + 1), url)
      if(c2 == -1){ break }
      store8(out, (c1 << 2) | (c2 >> 4), p)
      p += 1
      if(i + 2 < n){
         def v3 = load8(s, i + 2)
         if(v3 != 61){
            def c3 = _b64_unmap(v3, url)
            if(c3 != -1){
               store8(out, ((c2 & 15) << 4) | (c3 >> 2), p)
               p += 1
               if(i + 3 < n){
                  def v4 = load8(s, i + 3)
                  if(v4 != 61){
                     def c4 = _b64_unmap(v4, url)
                     if(c4 != -1){
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

fn encode64(str: data): str {
   "Encodes a byte string into Base64(RFC 4648 Section 4)."
   _encode64_internal(data, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/")
}

fn decode64(str: s): str {
   "Decodes a Base64 string into bytes(RFC 4648 Section 4)."
   _decode64_internal(s, false)
}

fn encode64_url(str: data): str {
   "Encodes a byte string into Base64 URL and Filename Safe(RFC 4648 Section 5)."
   _encode64_internal(data, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")
}

fn decode64_url(str: s): str {
   "Decodes a Base64 URL and Filename Safe string into bytes(RFC 4648 Section 5)."
   _decode64_internal(s, true)
}

fn _b32_unmap(int: c, bool: hex=false): int {
   if(hex){
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

fn _encode32_internal(str: data, str: alphabet, bool: padding=true): str {
   if(!is_str(data)){ return "" }
   def n = data.len
   mut out = str.Builder(max(16, ((n + 4) / 5) * 8 + 8))
   mut i = 0
   while(i < n){
      def b1 = load8(data, i) & 255
      def b2 = (i + 1 < n) ? (load8(data, i + 1) & 255) : 0
      def b3 = (i + 2 < n) ? (load8(data, i + 2) & 255) : 0
      def b4 = (i + 3 < n) ? (load8(data, i + 3) & 255) : 0
      def b5 = (i + 4 < n) ? (load8(data, i + 4) & 255) : 0
      out = str.builder_append(out, chr(load8(alphabet, b1 >> 3)))
      out = str.builder_append(out, chr(load8(alphabet, ((b1 & 7) << 2) | (b2 >> 6))))
      if(i + 1 < n){
         out = str.builder_append(out, chr(load8(alphabet, (b2 >> 1) & 31)))
         out = str.builder_append(out, chr(load8(alphabet, ((b2 & 1) << 4) | (b3 >> 4))))
      } elif(padding){
         out = str.builder_append(out, "======")
         break
      }
      if(i + 2 < n){ out = str.builder_append(out, chr(load8(alphabet, ((b3 & 15) << 1) | (b4 >> 7)))) } elif(padding){
         out = str.builder_append(out, "====")
         break
      }
      if(i + 3 < n){
         out = str.builder_append(out, chr(load8(alphabet, (b4 >> 2) & 31)))
         out = str.builder_append(out, chr(load8(alphabet, ((b4 & 3) << 3) | (b5 >> 5))))
      } elif(padding){
         out = str.builder_append(out, "===")
         break
      }
      if(i + 4 < n){ out = str.builder_append(out, chr(load8(alphabet, b5 & 31))) } elif(padding){
         out = str.builder_append(out, "=")
         break
      }
      i += 5
   }
   _base_builder_take(out)
}

fn _decode32_internal(str: s, bool: hex=false): str {
   if(!is_str(s)){ return "" }
   def n = s.len
   mut out = malloc(n)
   if(!out){ return "" }
   mut p = 0
   mut bits = 0
   mut val = 0
   mut i = 0
   while(i < n){
      def c = load8(s, i)
      if(c == 61){ break }
      def v = _b32_unmap(c, hex)
      if(v == -1){
         i += 1
         continue
      }
      val = (val << 5) | v
      bits += 5
      if(bits >= 8){
         store8(out, (val >> (bits - 8)) & 255, p)
         p += 1
         bits -= 8
      }
      i += 1
   }
   init_str(out, p)
   out
}

fn encode32(str: data): str {
   "Encodes a byte string into Base32(RFC 4648 Section 6)."
   _encode32_internal(data, "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567")
}

fn decode32(str: s): str {
   "Decodes a Base32 string into bytes(RFC 4648 Section 6)."
   _decode32_internal(s, false)
}

fn encode32_hex(str: data): str {
   "Encodes a byte string into Base32 with Hex Alphabet(RFC 4648 Section 7)."
   _encode32_internal(data, "0123456789ABCDEFGHIJKLMNOPQRSTUV")
}

fn decode32_hex(str: s): str {
   "Decodes a Base32 Hex string into bytes(RFC 4648 Section 7)."
   _decode32_internal(s, true)
}

fn encode16(str: data): str {
   "Encodes a byte string into Base16(Hex) (RFC 4648 Section 8)."
   if(!is_str(data)){ return "" }
   def n = data.len
   mut out = str.Builder(max(16, n * 2 + 8))
   def alphabet = "0123456789ABCDEF"
   mut i = 0
   while(i < n){
      def b = load8(data, i) & 255
      out = str.builder_append(out, chr(load8(alphabet, b >> 4)))
      out = str.builder_append(out, chr(load8(alphabet, b & 15)))
      i += 1
   }
   _base_builder_take(out)
}

fn decode16(str: s): str {
   "Decodes a Base16(Hex) string into bytes(RFC 4648 Section 8)."
   if(!is_str(s)){ return "" }
   def n = s.len
   mut out = malloc(n / 2)
   if(!out){ return "" }
   mut p, i = 0, 0
   while(i + 1 < n){
      def c1, c2 = load8(s, i), load8(s, i + 1)
      def v1, v2 = _base_hex_unmap(c1), _base_hex_unmap(c2)
      if(v1 != -1 && v2 != -1){
         store8(out, (v1 << 4) | v2, p)
         p += 1
      }
      i += 2
   }
   init_str(out, p)
   out
}
