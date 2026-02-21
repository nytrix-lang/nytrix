;; Keywords: str
;; String module.

module std.str (
   str_len, len, find, _str_eq, cstr_to_str, pad_start, startswith, endswith, atoi, split, strip,
   str_add, upper, lower, str_contains, join, str_replace, replace_all, to_hex, chr, repeat, ord,
   utf8_valid, utf8_len, ord_at
)
use std.core *
use std.core as core

fn str_len(s){
   "Returns the number of bytes in a string."
   if(!s){ return 0 }
   if(!is_str(s)){ return 0 }
   load64(s, -16)
}

fn len(s){
   "Alias for [[std.core::len]]. For strings, returns the number of bytes."
   core.len(s)
}

fn find(s, sub){
   "Returns the index of the first occurrence of `sub` in `s`, or -1."
   mut n = str_len(s)
   mut m = str_len(sub)
   if(m == 0){ return 0 }
   if(n < m){ return -1 }
   mut i = 0
   while(i + m <= n){
      mut j = 0
      while(j < m){
         if(load8(s, i + j) != load8(sub, j)){ break }
         j += 1
      }
      if(j == m){ return i }
      i += 1
   }
   return -1
}

fn _str_eq(a, b){
   "Returns true if two strings are equal."
   if(!is_str(a) || !is_str(b)){ return false }
   mut n = str_len(a)
   if n != str_len(b){ return false }
   mut i = 0
   while(i < n){
      if(load8(a, i) != load8(b, i)){ return false }
      i += 1
   }
   return true
}

fn cstr_to_str(p, offset=0){
   "Converts a C-string pointer to a Nytrix string. Optional offset skips bytes."
   if(!p){ return 0 }
   if(is_str(p)){
      if(offset == 0){ return p }
      return _substr(p, offset, str_len(p))
   }
   if(!is_int(offset)){ offset = 0 }

   mut n = 0
   while(load8(p, offset + n) != 0){ n += 1 }
   mut out = malloc(n + 1)
   if(!out){ return 0 }
   init_str(out, n)
   mut i = 0
   while(i < n){
      store8(out, load8(p, offset + i), i)
      i += 1
   }
   store8(out, 0, n)
   out
}

fn pad_start(s, width, pad=" "){
   "Left-pads string `s` to `width` using `pad` (default space)."
   mut n = str_len(s)
   if(n >= width){ return s }
   mut pad_len = str_len(pad)
   if(pad_len == 0){ return s }
   def total = width
   mut out = malloc(total + 1)
   if(!out){ return 0 }
   init_str(out, total)
   def pad_needed = width - n
   mut i = 0
   while(i < pad_needed){
      store8(out, load8(pad, i % pad_len), i)
      i += 1
   }
   mut j = 0
   while(j < n){
      store8(out, load8(s, j), pad_needed + j)
      j += 1
   }
   store8(out, 0, total)
   out
}

fn startswith(s, prefix){
   "Returns true if string `s` starts with `prefix`."
   if(!is_str(s) || !is_str(prefix)){ return false }
   mut n = str_len(prefix)
   if(str_len(s) < n){ return false }
   mut i = 0
   while(i < n){
      if(load8(s, i) != load8(prefix, i)){ return false }
      i += 1
   }
   return true
}

fn atoi(s){
   "Parses a decimal integer from string `s`."
   if(!is_str(s)){ return 0 }
   mut n = str_len(s)
   if(n == 0){ return 0 }
   mut i = 0
   mut sign = 1
   if(load8(s, 0) == 45){
      sign = -1
      i = 1
   }
   mut val = 0
   while(i < n){
      mut c = load8(s, i)
      if(c < 48 || c > 57){ break }
      val = val * 10 + (c - 48)
      i += 1
   }
   if(sign < 0){ val = 0 - val }
   val
}

fn _list_append(lst, v){
   "Internal: appends `v` to `lst`, growing capacity if needed."
   if(!is_list(lst)){ return lst }
   mut n = load64(lst, 0)
   def cap = load64(lst, 8)
   if(n >= cap){
      def newcap = (cap == 0) ? 8 : (cap * 2)
      def newp = list(newcap)
      store64(newp, load64(lst, -8), -8)
      mut i = 0
      while(i < n){
         store64(newp, load64(lst, 16 + i * 8), 16 + i * 8)
         i += 1
      }
      free(lst)
      lst = newp
   }
   store64(lst, v, 16 + n * 8)
   store64(lst, n + 1, 0)
   lst
}

fn _substr(s, start, stop){
   "Internal: substring helper with clamped byte indices."
   mut n = str_len(s)
   if(start < 0){ start = 0 }
   if(stop > n){ stop = n }
   if(start >= stop){ return "" }
   def len = stop - start
   mut out = malloc(len + 1)
   if(!out){ return 0 }
   init_str(out, len)
   mut i = 0
   while(i < len){
      store8(out, load8(s, start + i), i)
      i += 1
   }
   store8(out, 0, len)
   out
}

fn _is_ws(c){
   "Internal: returns true for ASCII whitespace byte values."
   if(c == 32 || c == 9 || c == 10 || c == 11 || c == 12 || c == 13){ return true }
   return false
}

fn strip(s){
   "Returns `s` without leading/trailing ASCII whitespace."
   if(!is_str(s)){ return "" }
   mut n = str_len(s)
   if(n == 0){ return "" }
   mut start = 0
   while(start < n && _is_ws(load8(s, start))){ start += 1 }
   mut end = n
   while(end > start && _is_ws(load8(s, end - 1))){ end -= 1 }
   _substr(s, start, end)
}

fn split(s, sep){
   "Splits string `s` by separator `sep` and returns a list of strings."
   if(!is_str(s)){ return list(0) }
   mut sep_len = str_len(sep)
   if(sep_len == 0){ return list(0) }
   mut out = list(8)
   mut n = str_len(s)
   mut i = 0
   mut start = 0
   while(i <= n - sep_len){
      mut j = 0
      mut is_match = 1
      while(j < sep_len){
        if(load8(s, i + j) != load8(sep, j)){
          is_match = 0
          break
        }
         j += 1
      }
      if(is_match){
         out = _list_append(out, _substr(s, start, i))
         i = i + sep_len
         start = i
      } else {
         i += 1
      }
   }
   out = _list_append(out, _substr(s, start, n))
   out
}

fn str_add(a, b){
   "Concatenates two strings."
   if(!is_str(a)){ return b }
   if(!is_str(b)){ return a }
   def n1 = str_len(a)
   def n2 = str_len(b)
   def total = n1 + n2
   mut out = malloc(total + 1)
   if(!out){ return 0 }
   init_str(out, total)
   mut i = 0
   while(i < n1){
      store8(out, load8(a, i), i)
      i += 1
   }
   mut j = 0
   while(j < n2){
      store8(out, load8(b, j), n1 + j)
      j += 1
   }
   store8(out, 0, total)
   out
}

fn upper(s){
   "Converts string `s` to uppercase."
   if(!is_str(s)){ return s }
   mut n = str_len(s)
   mut out = malloc(n + 1)
   init_str(out, n)
   mut i = 0
   while(i < n){
      mut c = load8(s, i)
      if(c >= 97 && c <= 122){ c = c - 32 }
      store8(out, c, i)
      i += 1
   }
   store8(out, 0, n)
   out
}

fn lower(s){
   "Converts string `s` to lowercase."
   if(!is_str(s)){ return s }
   mut n = str_len(s)
   mut out = malloc(n + 1)
   init_str(out, n)
   mut i = 0
   while(i < n){
      mut c = load8(s, i)
      if(c >= 65 && c <= 90){ c = c + 32 }
      store8(out, c, i)
      i += 1
   }
   store8(out, 0, n)
   out
}

fn endswith(s, suffix){
   "Returns true if string `s` ends with `suffix`."
   if(!is_str(s) || !is_str(suffix)){ return false }
   mut n = str_len(s)
   def m = str_len(suffix)
   if(n < m){ return false }
   mut i = 0
   while(i < m){
      if(load8(s, n - m + i) != load8(suffix, i)){ return false }
      i += 1
   }
   return true
}

fn str_contains(s, sub){
   "Returns true if string `s` contains `sub`."
   find(s, sub) != -1
}

fn join(items, sep){
   "Joins a list of strings using separator `sep`."
   if(!is_list(items)){ return "" }
   if(!is_str(sep)){ return "" }
   mut n = core.len(items)
   if(n == 0){ return "" }
   mut out = get(items, 0)
   mut i = 1
   while(i < n){
      out = str_add(out, sep)
      out = str_add(out, get(items, i))
      i += 1
   }
   out
}

fn str_replace(s, old, new){
   "Replaces all occurrences of `old` in `s` with `new`."
   if(!is_str(s) || !is_str(old) || !is_str(new)){ return s }
   if(str_len(old) == 0){ return s }
   def parts = split(s, old)
   join(parts, new)
}

fn replace_all(s, old, new){
   "Alias for [[std.str::str_replace]]."
   str_replace(s, old, new)
}

fn to_hex(n, width=0){
   "Converts an integer `n` to its hexadecimal string representation."
   def hex_chars = "0123456789abcdef"
   mut s = ""
   if(n == 0){ s = "0" }
   else {
      mut val = n
      while(val > 0){
         def nibble = (val & 0xF)
         def char_code = load8(hex_chars, nibble)
         def char_str_ptr = malloc(2) ; 1 byte for char, 1 for null terminator
         store8(char_str_ptr, char_code, 0)
         store8(char_str_ptr, 0, 1)
         def char_str = cstr_to_str(char_str_ptr)
         free(char_str_ptr)
         s = str_add(char_str, s)
         val = (val >> 4)
      }
   }
   if(width > 0){
      def slen = str_len(s)
      if(width > slen){
         mut pad = width - slen
         while(pad > 0){
            s = str_add("0", s)
            pad -= 1
         }
      }
   }
   return s
}

fn chr(code){
   "Returns a single-character string from an integer Unicode code point."
   if(code < 0 || code > 1114111){ return "" }
   def char_buf = malloc(5)
   if(!char_buf){ return "" }
   mut len = 0
   if(code <= 127){
      store8(char_buf, code, 0)
      len = 1
   } else if(code <= 2047){
      store8(char_buf, 192 | (code >> 6), 0)
      store8(char_buf, 128 | (code & 63), 1)
      len = 2
   } else if(code <= 65535){
      store8(char_buf, 224 | (code >> 12), 0)
      store8(char_buf, 128 | ((code >> 6) & 63), 1)
      store8(char_buf, 128 | (code & 63), 2)
      len = 3
   } else {
      store8(char_buf, 240 | (code >> 18), 0)
      store8(char_buf, 128 | ((code >> 12) & 63), 1)
      store8(char_buf, 128 | ((code >> 6) & 63), 2)
      store8(char_buf, 128 | (code & 63), 3)
      len = 4
   }
   store8(char_buf, 0, len)
   def s = cstr_to_str(char_buf)
   free(char_buf)
   return s
}

fn repeat(s, n){
   "Returns string `s` repeated `n` times."
   if(!is_str(s) || n < 0){ return "" }
   if(n == 0){ return "" }
   def slen = str_len(s)
   def total_len = slen * n
   mut out = malloc(total_len + 1)
   if(!out){ return "" }
   init_str(out, total_len)
   mut i = 0
   while(i < n){
      mut j = 0
      while(j < slen){
         store8(out, load8(s, j), i * slen + j)
         j += 1
      }
      i += 1
   }
   store8(out, 0, total_len)
   return out
}

fn _is_utf8_cont(c){
   "Internal: continuation byte check."
   c >= 128 && c <= 191
}

fn _utf8_seq_len(s, i, n){
   "Internal: returns UTF-8 sequence width at byte offset `i`; -1 if invalid."
   if(i < 0 || i >= n){ return -1 }
   def b0 = load8(s, i)
   if(b0 < 128){ return 1 }

   if(b0 >= 194 && b0 <= 223){
      if(i + 1 >= n){ return -1 }
      def b1 = load8(s, i + 1)
      if(!_is_utf8_cont(b1)){ return -1 }
      return 2
   }

   if(b0 >= 224 && b0 <= 239){
      if(i + 2 >= n){ return -1 }
      def b1 = load8(s, i + 1)
      def b2 = load8(s, i + 2)
      if(!_is_utf8_cont(b1) || !_is_utf8_cont(b2)){ return -1 }
      ;; Reject overlong forms and UTF-16 surrogate range.
      if(b0 == 224 && b1 < 160){ return -1 }
      if(b0 == 237 && b1 >= 160){ return -1 }
      return 3
   }

   if(b0 >= 240 && b0 <= 244){
      if(i + 3 >= n){ return -1 }
      def b1 = load8(s, i + 1)
      def b2 = load8(s, i + 2)
      def b3 = load8(s, i + 3)
      if(!_is_utf8_cont(b1) || !_is_utf8_cont(b2) || !_is_utf8_cont(b3)){ return -1 }
      ;; Reject overlong forms and code points beyond U+10FFFF.
      if(b0 == 240 && b1 < 144){ return -1 }
      if(b0 == 244 && b1 > 143){ return -1 }
      return 4
   }

   -1
}

fn _utf8_decode_at(s, i, w){
   "Internal: decodes UTF-8 sequence at `i` with known width `w`."
   def b0 = load8(s, i)
   if(w == 1){ return b0 }
   if(w == 2){
      def b1 = load8(s, i + 1)
      return ((b0 & 31) << 6) | (b1 & 63)
   }
   if(w == 3){
      def b1 = load8(s, i + 1)
      def b2 = load8(s, i + 2)
      return ((b0 & 15) << 12) | ((b1 & 63) << 6) | (b2 & 63)
   }
   if(w == 4){
      def c1 = load8(s, i + 1)
      def c2 = load8(s, i + 2)
      def c3 = load8(s, i + 3)
      return ((b0 & 7) << 18) | ((c1 & 63) << 12) | ((c2 & 63) << 6) | (c3 & 63)
   }
   0
}

fn utf8_valid(s){
   "Returns true if `s` is valid UTF-8."
   if(!is_str(s)){ return false }
   def n = str_len(s)
   mut i = 0
   while(i < n){
      def w = _utf8_seq_len(s, i, n)
      if(w < 1){ return false }
      i = i + w
   }
   true
}

fn utf8_len(s){
   "Returns the number of UTF-8 code points in `s` (invalid bytes count as one)."
   if(!is_str(s)){ return 0 }
   def n = str_len(s)
   mut i = 0
   mut count = 0
   while(i < n){
      def w = _utf8_seq_len(s, i, n)
      if(w < 1){ i += 1 }
      else { i = i + w }
      count += 1
   }
   count
}

fn ord_at(s, idx=0){
   "Returns Unicode code point at code-point index `idx` (supports negative indices)."
   if(!is_str(s)){ return 0 }
   if(!is_int(idx)){ idx = 0 }
   mut total = utf8_len(s)
   if(idx < 0){ idx = total + idx }
   if(idx < 0 || idx >= total){ return 0 }

   def n = str_len(s)
   mut i = 0
   mut pos = 0
   while(i < n){
      def w = _utf8_seq_len(s, i, n)
      if(pos == idx){
         if(w < 1){ return load8(s, i) }
         return _utf8_decode_at(s, i, w)
      }
      if(w < 1){ i += 1 }
      else { i = i + w }
      pos += 1
   }
   0
}

fn ord(s){
   "Returns the Unicode code point of the first character in `s`."
   ord_at(s, 0)
}

if(comptime{__main()}){
    use std.core *
    use std.str.str *
    use std.str *

    def s = "hello"
    assert(str_len(s) == 5, "str_len")
    assert(find(s, "ell") == 1, "find substring")
    assert(find(s, "zzz") == -1, "find missing")

    assert(_str_eq("a", "a"), "_str_eq true")
    assert(!_str_eq("a", "b"), "_str_eq false")

    assert(pad_start("7", 3, "0") == "007", "pad_start")
    assert(startswith("hello", "he"), "startswith")
    assert(endswith("hello", "lo"), "endswith")

    assert(atoi("123") == 123, "atoi")
    assert(atoi("-7") == -7, "atoi negative")

    mut parts = split("a,b,c", ",")
    assert(len(parts) == 3, "split count")
    assert(get(parts, 0) == "a", "split first")
    assert(get(parts, 2) == "c", "split last")

    assert(strip("  hi \n") == "hi", "strip")
    assert(str_add("he", "llo") == "hello", "str_add")
    assert(upper("heLlo") == "HELLO", "upper")
    assert(lower("HeLLo") == "hello", "lower")
    assert(str_contains("hello", "ell"), "str_contains")

    mut items = ["a", "b", "c"]
    assert(join(items, ",") == "a,b,c", "join")

    assert(str_replace("a-b-a", "a", "x") == "x-b-x", "str_replace")
    assert(replace_all("a-b-a", "-", ":") == "a:b:a", "replace_all")

    def euro = chr(8364) ;; U+20AC
    def grin = chr(128512) ;; U+1F600
    def mixed = "A" + euro + grin
    assert(ord("A") == 65, "ord ascii")
    assert(ord(euro) == 8364, "ord unicode bmp")
    assert(ord(grin) == 128512, "ord unicode astral")
    assert(utf8_len(mixed) == 3, "utf8_len code points")
    assert(utf8_valid(mixed), "utf8_valid true")
    assert(ord_at(mixed, 0) == 65, "ord_at first")
    assert(ord_at(mixed, 1) == 8364, "ord_at middle")
    assert(ord_at(mixed, -1) == 128512, "ord_at negative index")
    assert(_str_eq(utf8_slice(mixed, 0, 2), "A" + euro), "utf8_slice basic")
    assert(_str_eq(utf8_slice(mixed, 0, 3, 2), "A" + grin), "utf8_slice step")
    assert(_str_eq(utf8_slice(mixed, -2, -1), euro), "utf8_slice negative bounds")
    assert(_str_eq(slice(mixed, 1, 3), euro + grin), "generic slice utf8")

    def bad = malloc(2)
    init_str(bad, 1)
    store8(bad, 255, 0)
    store8(bad, 0, 1)
    assert(!utf8_valid(bad), "utf8_valid false")
    assert(utf8_len(bad) == 1, "utf8_len invalid byte fallback")

    def s = "abcdef"
    assert(_str_eq(str_slice(s, 0, 3), "abc"), "slice start")
    assert(_str_eq(str_slice(s, 1, 5, 2), "bd"), "slice step")
    assert(_str_eq(str_slice(s, -3, -1), "de"), "slice negative")
    assert(_str_eq(str_slice(s, 0, 0), ""), "slice empty")

    print("✓ std.str tests passed")
}
