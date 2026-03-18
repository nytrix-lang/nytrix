;; Keywords: platform window backend linux x11 common shared
;; Portable X11 utility operations shared by the native Linux window backend.
module std.os.ui.window.platform.linux.x11.common(encodeUTF8, encodeUTF8String, decodeUTF8, convertLatin1toUTF8, parseUriList, dup_string)
use std.core
use std.core.str as str

fn _decode_utf8_offset(int: count): int {
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

fn _hex_value(int: c): int {
   if(c >= 48 && c <= 57){ return c - 48 }
   if(c >= 65 && c <= 70){ return c - 55 }
   if(c >= 97 && c <= 102){ return c - 87 }
   -1
}

fn _percent_decode_uri(any: line): any {
   if(!is_str(line)){ return "" }
   def n = line.len
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

fn encodeUTF8(any: s, int: codepoint): int {
   "Encode one Unicode codepoint into a caller-provided UTF-8 byte buffer."
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

fn encodeUTF8String(int: codepoint): any {
   "Returns a Ny string containing the UTF-8 sequence for `codepoint`."
   def out = malloc(5)
   if(!out){ return 0 }
   def n = encodeUTF8(out, codepoint)
   store8(out, 0, n)
   init_str(out, n)
   out
}

fn decodeUTF8(any: s, int: start=0): list {
   "Decode one UTF-8 sequence and return `[codepoint, next_index]`."
   if(!is_str(s)){ return [0, start] }
   def n = s.len
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

fn convertLatin1toUTF8(any: source): any {
   "Convert a Latin-1 byte string to UTF-8."
   if(!is_str(source)){ return "" }
   def n = source.len
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
      dst += encodeUTF8(out + dst, load8(source, src))
      src += 1
   }
   store8(out, 0, dst)
   init_str(out, dst)
   out
}

fn parseUriList(any: text): list {
   "Parse a text/uri-list payload and return decoded filesystem paths."
   if(!is_str(text)){ return list(0) }
   mut paths = list(8)
   def n = text.len
   mut i = 0
   while(i < n){
      while(i < n && (load8(text, i) == 10 || load8(text, i) == 13)){ i += 1 }
      if(i >= n){ break }
      def start = i
      while(i < n && load8(text, i) != 10 && load8(text, i) != 13){ i += 1 }
      mut line = str.str_slice(text, start, i)
      if(line.len == 0){ continue }
      if(load8(line, 0) == 35){ continue }
      if(str.startswith(line, "file://")){
         line = str.str_slice(line, 7, line.len)
         mut slash = 0
         while(slash < line.len && load8(line, slash) != 47){ slash += 1 }
         if(slash < line.len){ line = str.str_slice(line, slash, line.len) }
      }
      paths = paths.append(_percent_decode_uri(line))
   }
   paths
}

fn dup_string(any: source): any {
   "Copy a Ny string into owned NUL-terminated string storage."
   if(!is_str(source)){ return "" }
   def n = source.len
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
