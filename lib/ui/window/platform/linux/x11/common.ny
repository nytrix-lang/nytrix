;; Keywords: ui window glfw port common
;; Direct Ny ports of portable GLFW C helpers from init.c and x11_window.c.

module std.ui.window.platform.linux.x11.common (
   _glfwEncodeUTF8, _glfwEncodeUTF8String,
   decodeUTF8, convertLatin1toUTF8,
   _glfwParseUriList, _glfw_strdup,
   _glfw_min, _glfw_max
)

use std.core *
use std.str as str

fn _decode_utf8_offset(count){
   match count {
      1 -> 0x00000000
      2 -> 0x00003080
      3 -> 0x000e2080
      4 -> 0x03c82080
      5 -> 0xfa082080
      6 -> 0x82082080
      _ -> 0
   }
}

fn _hex_value(c){
   if(c >= 48 && c <= 57){ return c - 48 }
   if(c >= 65 && c <= 70){ return c - 55 }
   if(c >= 97 && c <= 102){ return c - 87 }
   -1
}

fn _percent_decode_uri(line){
   if(!is_str(line)){ return "" }
   def n = str.len(line)
   def out = malloc(n + 1)
   if(!out){ return 0 }

   mut src = 0
   mut dst = 0
   while(src < n){
      if(load8(line, src) == 37 && src + 2 < n){
         def hi = _hex_value(load8(line, src + 1))
         def lo = _hex_value(load8(line, src + 2))
         if(hi >= 0 && lo >= 0){
         store8(out, hi * 16 + lo, dst)
         src += 3
         dst += 1
         continue
         }
      }

      store8(out, load8(line, src), dst)
      src += 1
      dst += 1
   }

   store8(out, 0, dst)
   init_str(out, dst)
   out
}

fn _glfwEncodeUTF8(s, codepoint){
   "Direct port of GLFW's `_glfwEncodeUTF8` into a caller-provided byte buffer."
   mut count = 0

   if(codepoint < 0x80){
      store8(s, codepoint, count)
      count += 1
   } elif(codepoint < 0x800){
      store8(s, bor(bshr(codepoint, 6), 0xc0), count)
      count += 1
      store8(s, bor(band(codepoint, 0x3f), 0x80), count)
      count += 1
   } elif(codepoint < 0x10000){
      store8(s, bor(bshr(codepoint, 12), 0xe0), count)
      count += 1
      store8(s, bor(band(bshr(codepoint, 6), 0x3f), 0x80), count)
      count += 1
      store8(s, bor(band(codepoint, 0x3f), 0x80), count)
      count += 1
   } elif(codepoint < 0x110000){
      store8(s, bor(bshr(codepoint, 18), 0xf0), count)
      count += 1
      store8(s, bor(band(bshr(codepoint, 12), 0x3f), 0x80), count)
      count += 1
      store8(s, bor(band(bshr(codepoint, 6), 0x3f), 0x80), count)
      count += 1
      store8(s, bor(band(codepoint, 0x3f), 0x80), count)
      count += 1
   }

   count
}

fn _glfwEncodeUTF8String(codepoint){
   "Returns a Ny string containing the UTF-8 sequence for `codepoint`."
   def out = malloc(5)
   if(!out){ return 0 }
   def n = _glfwEncodeUTF8(out, codepoint)
   store8(out, 0, n)
   init_str(out, n)
   out
}

fn decodeUTF8(s, start=0){
   "Direct Ny adaptation of GLFW's `decodeUTF8`; returns `[codepoint, next_index]`."
   if(!is_str(s)){ return [0, start] }
   def n = str.len(s)
   if(start < 0 || start >= n){ return [0, start] }

   mut codepoint = 0
   mut count = 0
   mut i = start

   while(i < n){
      codepoint = (codepoint << 6) + load8(s, i)
      i += 1
      count += 1
      if(i >= n){ break }
      if(band(load8(s, i), 0xc0) != 0x80){ break }
   }

   [codepoint - _decode_utf8_offset(count), i]
}

fn convertLatin1toUTF8(source){
   "Direct Ny port of GLFW's `convertLatin1toUTF8`."
   if(!is_str(source)){ return "" }
   def n = str.len(source)
   mut size = 1
   mut i = 0
   while(i < n){
      size += band(load8(source, i), 0x80) ? 2 : 1
      i += 1
   }

   def out = malloc(size)
   if(!out){ return 0 }
   mut src = 0
   mut dst = 0
   while(src < n){
      dst += _glfwEncodeUTF8(out + dst, load8(source, src))
      src += 1
   }
   store8(out, 0, dst)
   init_str(out, dst)
   out
}

fn _glfwParseUriList(text){
   "Ny adaptation of GLFW's `_glfwParseUriList`; returns a list of decoded paths."
   if(!is_str(text)){ return list(0) }
   mut paths = list(8)
   def n = str.len(text)
   mut i = 0

   while(i < n){
      while(i < n && (load8(text, i) == 10 || load8(text, i) == 13)){ i += 1 }
      if(i >= n){ break }

      def start = i
      while(i < n && load8(text, i) != 10 && load8(text, i) != 13){ i += 1 }
      mut line = str.str_slice(text, start, i)
      if(str.len(line) == 0){ continue }
      if(load8(line, 0) == 35){ continue }

      if(str.startswith(line, "file://")){
         line = str.str_slice(line, 7, str.len(line))
         mut slash = 0
         while(slash < str.len(line) && load8(line, slash) != 47){ slash += 1 }
         if(slash < str.len(line)){ line = str.str_slice(line, slash, str.len(line)) }
      }

      paths = append(paths, _percent_decode_uri(line))
   }

   paths
}

fn _glfw_strdup(source){
   "Direct Ny port of GLFW's `_glfw_strdup`."
   if(!is_str(source)){ return "" }
   def n = str.len(source)
   def out = malloc(n + 1)
   if(!out){ return 0 }
   init_str(out, n)
   mut i = 0
   while(i < n){
      store8(out, load8(source, i), i)
      i += 1
   }
   store8(out, 0, n)
   out
}

fn _glfw_min(a, b){
   "Direct Ny port of GLFW's `_glfw_min`."
   a < b ? a : b
}

fn _glfw_max(a, b){
   "Direct Ny port of GLFW's `_glfw_max`."
   a > b ? a : b
}

if(comptime{ __main() }){
   use std.core.error *

   fn _latin1_bytes(bytes){
      def out = malloc(len(bytes) + 1)
      init_str(out, len(bytes))
      mut i = 0
      while(i < len(bytes)){
         store8(out, get(bytes, i), i)
         i += 1
      }
      store8(out, 0, len(bytes))
      out
   }

   assert(_glfw_min(3, 1) == 1, "_glfw_min")
   assert(_glfw_max(3, 1) == 3, "_glfw_max")

   def euro = _glfwEncodeUTF8String(0x20ac)
   assert(euro == str.chr(0x20ac), "_glfwEncodeUTF8String")

   def decoded = decodeUTF8(euro, 0)
   assert(get(decoded, 0) == 0x20ac, "decodeUTF8 codepoint")
   assert(get(decoded, 1) == str.len(euro), "decodeUTF8 next index")

   def latin1 = _latin1_bytes([65, 233])
   assert(convertLatin1toUTF8(latin1) == ("A" + str.chr(233)), "convertLatin1toUTF8")

   def parsed = _glfwParseUriList("file:///build/cache/a%20b\r\n#ignored\nfile://host/home/e/test")
   assert(len(parsed) == 2, "_glfwParseUriList count")
   assert(get(parsed, 0) == "/build/cache/a b", "_glfwParseUriList first")
   assert(get(parsed, 1) == "/home/e/test", "_glfwParseUriList second")
}
