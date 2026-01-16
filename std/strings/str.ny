;; Keywords: strings str
;; Strings Str module.

module std.strings.str (
   str_clone, cstr_to_str, str_len, char_at, str_slice, find, str_contains, split,
   join, partition, replace_all, count, strip, lstrip, rstrip, upper, lower, repeat,
   splitlines, pad_start, pad_end, zfill, chr, ord, atoi, startswith, endswith, _str_eq
)

fn str_clone(s){
   "Creates a **deep copy** of string `s` in a new memory block."
   def n = str_len(s)
   def p = __malloc(n + 1)
   __init_str(p, n)
   __memcpy(p, s, n)
   __store8_idx(p, n, 0)
   p
}

fn cstr_to_str(s, off=0){
   "Converts a null-terminated C-string to a Nytrix **str** object, starting from offset `off`."
   if(!is_int(off)){ off = 0 }
   def n = 0
   while(__load8_idx(s, off + n) != 0){ n = n + 1 }
   def p = __malloc(n + 1)
   __init_str(p, n)
   __memcpy(p, s + off, n)
   __store8_idx(p, n, 0)
   p
}

fn str_len(s){
   "Returns the number of bytes in string `s` (excluding the null terminator)."
   if(s == 0){ return 0 }
   return __load64_idx(s, -16)
}

fn str_add(a, b){
  "Concatenates two strings `a` and `b` returning a new string object."
  __str_concat(a, b)
}


fn char_at(s, i){
   "Returns the character at index `i` as a single-character string."
   str_slice(s, i, i + 1, 1)
}

fn str_slice(s, start, stop, step=1){
   "Returns a **slice** (substring) of `s` from `start` to `stop` (exclusive) with an optional `step` value."
   def len = str_len(s)
   if(start < 0){ start = len + start }
   if(stop < 0){ stop = len + stop }
   if(step > 0){
      if(start < 0){ start = 0 }
      if(stop > len){ stop = len }
      if(start >= stop){ return "" }
   } else {
      if(start >= len){ start = len - 1 }
      if(stop < -1){ stop = -1 }
      if(start <= stop){ return "" }
   }
   def out_len = 0
   if(step > 0){
      out_len = (stop - start + step - 1) / step
   } else {
      out_len = (start - stop - step - 1) / (0 - step)
   }
   if(out_len <= 0){ return "" }
   def out = __malloc(out_len + 1)
   __init_str(out, out_len)
   def i = start
   def oi = 0
   if(step > 0){
      while(i < stop){
         __store8_idx(out, oi, __load8_idx(s, i))
         oi += 1
         i += step
      }
   } else {
      while(i > stop){
         __store8_idx(out, oi, __load8_idx(s, i))
         oi += 1
         i += step
      }
   }
   __store8_idx(out, oi, 0)
   out
}

fn find(s, sub){
   "Search for `sub` in string `s`. Returns the **first** starting index of `sub`, or `-1` if not found."
   def ls = str_len(s)
   def lp = str_len(sub)
   if(ls < lp){ return -1 }
   if(lp == 0){ return 0 }
   def i = 0
   while(i <= ls - lp){
      def j = 0
      def is_match = 1
      while(j < lp){
         def char_s = __load8_idx(s, i + j)
         def char_sub = __load8_idx(sub, j)
         if(char_s != char_sub){
            is_match = 0
            break
         }
         j += 1
      }
      if(is_match == 1){ return i }
      i += 1
   }
   -1
}

fn str_contains(s, sub){
  "Returns **true** if `sub` is present within string `s`."
  find(s, sub) >= 0
}

fn split(s, sep){
   "Splits string `s` into a [[std.core::list]] of strings using `sep` as the delimiter."
   def res = list(8)
   def start = 0
   def n = str_len(s)
   def sn = str_len(sep)
   if(sn == 0){ return res }
   def i = 0
   while(i <= n - sn){
      def is_match = 1  def j = 0
      while(j < sn){
         if(__load8_idx(s, i + j) != __load8_idx(sep, j)){
            is_match = 0
            break
         }
         j += 1
      }
      if(is_match){
         res = append(res, str_slice(s, start, i, 1))
         start = i + sn
         i = start
      } else {
         i += 1
      }
   }
   res = append(res, str_slice(s, start, n, 1))
   res
}

fn join(xs, sep){
   "Concatenates elements of [[std.core::list]] `xs` into a single string, separated by `sep`."
   def n = list_len(xs)
   if (n == 0) { "" }
   elif (n == 1) {
      def s = get(xs, 0)
      if(is_int(s)) { to_str(s) } else { s }
   } else {
      def res = get(xs, 0)
      if(is_int(res)) { res = to_str(res) }
      def i = 1
      while(i < n){
         def s = get(xs, i)
         res = f"{res}{sep}{case is_int(s) { true -> to_str(s) _ -> s }}"
         i += 1
      }
      res
   }
}

fn partition(s, sep){
   "Splits string `s` at the **first** occurrence of `sep`. Returns a 3-item list: `[before, sep, after]`."
   def idx = find(s, sep)
   if(idx < 0){ return [s, "", ""] }
   def sn = str_len(sep)
   return [str_slice(s, 0, idx, 1), sep, str_slice(s, idx + sn, str_len(s), 1)]
}

fn replace_all(s, old, nw){
   "Return a new string with all occurrences of `old` replaced by `nw`."
   def parts = split(s, old)
   join(parts, nw)
}

fn count(s, sub){
   "Returns the number of non-overlapping occurrences of substring `sub` in string `s`."
   def n = str_len(s)
   def m = str_len(sub)
   if(m == 0){ return 0 }
   def res = 0
   def i = 0
   while(i <= n - m){
      def is_match = 1
      def j = 0
      while(j < m){
         if(__load8_idx(s, i + j) != __load8_idx(sub, j)){
            is_match = 0
            break
         }
         j += 1
      }
      if(is_match == 1){
         res += 1
         i += m
      } else {
         i += 1
      }
   }
   res
}

fn strip(s){
   "Returns a copy of string `s` with **leading and trailing** whitespace removed."
   if(s == 0){ return "" }
   def n = str_len(s)
   def start = 0
   while(start < n){
      def c = __load8_idx(s, start)
      if(c != 32 && c != 10 && c != 13 && c != 9){ break }
      start += 1
   }
   if(start == n){ return "" }
   def end = n - 1
   while(end > start){
      def c = __load8_idx(s, end)
      if(c != 32 && c != 10 && c != 13 && c != 9){ break }
      end -= 1
   }
   str_slice(s, start, end + 1, 1)
}

fn lstrip(s){
   "Returns a copy of string `s` with **leading** whitespace removed."
   if(s == 0){ return "" }
   def n = str_len(s)
   def start = 0
   while(start < n){
      def c = __load8_idx(s, start)
      if(c != 32 && c != 10 && c != 13 && c != 9){ break }
      start += 1
   }
   str_slice(s, start, n, 1)
}

fn rstrip(s){
   "Returns a copy of string `s` with **trailing** whitespace removed."
   if(s == 0){ return "" }
   def n = str_len(s)
   def end = n - 1
   while(end >= 0){
      def c = __load8_idx(s, end)
      if(c != 32 && c != 10 && c != 13 && c != 9){ break }
      end -= 1
   }
   str_slice(s, 0, end + 1, 1)
}

fn upper(s){
   "Returns a copy of string `s` with all lowercase characters converted to **uppercase**."
   def n = str_len(s)
   def out = __malloc(n + 1)
   __init_str(out, n)
   def i = 0
   while(i < n){
      def c = __load8_idx(s, i)
      if(c >= 97 && c <= 122){ __store8_idx(out, i, c - 32) } else { __store8_idx(out, i, c) }
      i += 1
   }
   __store8_idx(out, n, 0)
   out
}

fn lower(s){
   "Returns a copy of string `s` with all uppercase characters converted to **lowercase**."
   def n = str_len(s)
   def out = __malloc(n + 1)
   __init_str(out, n)
   def i = 0
   while(i < n){
      def c = __load8_idx(s, i)
      if(c >= 65 && c <= 90){ __store8_idx(out, i, c + 32) } else { __store8_idx(out, i, c) }
      i += 1
   }
   __store8_idx(out, n, 0)
   out
}

fn repeat(s, n){
   "Returns a new string consisting of `s` repeated `n` times."
   if(n <= 0){ return "" }
   def res = ""
   def i = 0
   while(i < n){
      res = f"{res}{s}"
      i += 1
   }
   res
}

fn splitlines(s){
   "Splits string `s` at newline characters. Returns a [[std.core::list]] of lines."
   split(s, "\n")
}

fn pad_start(s, width, fill=" "){
   "Pads string `s` on the left with `fill` until it reaches `width`."
   if(fill == 0){ fill = " " }
   def l = str_len(s)
   if(l >= width){ return s }
   def diff = width - l
   def out = ""
   def i = 0
   while(i < diff){ out = f"{out}{fill}" i += 1 }
   f"{out}{s}"
}

fn pad_end(s, width, fill=" "){
   "Pads string `s` on the right with `fill` until it reaches `width`."
   if(fill == 0){ fill = " " }
   def l = str_len(s)
   if(l >= width){ return s }
   def diff = width - l
   def out = s
   def i = 0
   while(i < diff){ out = f"{out}{fill}" i += 1 }
   out
}

fn zfill(s, width){
   "Pads string `s` with zeros on the left until it reaches `width`. Handles leading sign characters gracefully."
   def l = str_len(s)
   if(l >= width){ return s }
   if(__load8_idx(s, 0) == 45){
      def zs = pad_start(str_slice(s, 1, l, 1), width - 1, "0")
      f"-{zs}"
   } else {
      pad_start(s, width, "0")
   }
}

fn chr(code){
   "Returns a single-character string containing the ASCII/Unicode character with code `code`."
   def p = __malloc(2)
   __init_str(p, 1)
   __store8_idx(p, 0, code)
   __store8_idx(p, 1, 0)
   p
}

fn ord(s){
   "Returns the numeric **ASCII** value of the first character in string `s`."
   __load8_idx(s, 0)
}

fn to_str(n){
   "Converts integer `n` to its decimal string representation."
   __to_str(n)
}

fn atoi(s){
   "Parses a decimal integer from string `s`. Returns `0` on parsing failure."
   if(s == 0){ return 0 }
   def n = str_len(s)
   if(n == 0){ return 0 }
   def i = 0
   while(i < n){ ; Skip whitespace
      def c = __load8_idx(s, i)
      if(c != 32 && c != 9 && c != 10 && c != 13){ break }
      i += 1
   }
   if(i == n){ return 0 }
   def sign = 1
   def c = __load8_idx(s, i)
   if(c == 45){ sign = -1 i += 1 } ; '-'
   else { if(c == 43){ i += 1 } } ; '+'
   def res = 0
   while(i < n){
      def c = __load8_idx(s, i)
      if(c < 48 || c > 57){ break }
      res = res * 10 + (c - 48)
      i += 1
   }
   res * sign
}

fn startswith(s, prefix){
   "Returns **true** if string `s` begins with the `prefix` string."
   def n = str_len(prefix)
   if(str_len(s) < n){ return false }
   def i = 0
   while(i < n){
      if(__load8_idx(s, i) != __load8_idx(prefix, i)){ return false }
      i += 1
   }
   true
}

fn endswith(s, suffix){
   "Returns **true** if string `s` ends with the `suffix` string."
   def n = str_len(suffix)
   def len = str_len(s)
   if(len < n){ return false }
   def start = len - n
   def i = 0
   while(i < n){
      if(__load8_idx(s, start + i) != __load8_idx(suffix, i)){ return false }
      i += 1
   }
   true
}

fn _str_eq(s1, s2){
  "Performs a byte-by-byte equality check between `s1` and `s2`."
  def n1 = str_len(s1)
  def n2 = str_len(s2)
  if(n1 != n2){ return false }
  def i = 0
  while(i < n1){
     if(__load8_idx(s1, i) != __load8_idx(s2, i)){ return false }
     i += 1
  }
  true
}