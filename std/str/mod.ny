;; Keywords: str
;; String module.

use std.core *
module std.str (
   str_len, find, _str_eq, cstr_to_str, pad_start, startswith, endswith, atoi, split, strip,
   str_add, upper, lower, str_contains, join, str_replace, replace_all
)

fn str_len(s){
   "Returns the number of bytes in a string."
   if(!s){ return 0 }
   if(!is_ptr(s)){ return 0 }
   load64(s, -16)
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
         j = j + 1
      }
      if(j == m){ return i }
      i = i + 1
   }
   return -1
}

fn _str_eq(a, b){
   "Returns true if two strings are equal."
   if(!is_str(a) || !is_str(b)){ return false }
   mut n = str_len(a)
   if(__eq(n, str_len(b)) == false){ return false }
   mut i = 0
   while(i < n){
      if(load8(a, i) != load8(b, i)){ return false }
      i = i + 1
   }
   return true
}

fn cstr_to_str(p, offset=0){
   "Converts a C-string pointer to a Nytrix string. Optional offset skips bytes."
   if(!p){ return 0 }
   if(is_str(p)){ return p } ; Handle case where it's already a Nytrix string
   
   mut n = 0
   while(load8(p, offset + n) != 0){ n = n + 1 }
   mut out = malloc(n + 1)
   if(!out){ return 0 }
   init_str(out, n)
   mut i = 0
   while(i < n){
      store8(out, load8(p, offset + i), i)
      i = i + 1
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
      i = i + 1
   }
   mut j = 0
   while(j < n){
      store8(out, load8(s, j), pad_needed + j)
      j = j + 1
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
      i = i + 1
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
      i = i + 1
   }
   if(sign < 0){ val = 0 - val }
   val
}

fn _list_append(lst, v){
   if(!is_list(lst)){ return lst }
   mut n = load64(lst, 0)
   def cap = load64(lst, 8)
   if(n >= cap){
      def newcap = eq(cap, 0) ? 8 : (cap * 2)
      def newp = list(newcap)
      store64(newp, load64(lst, -8), -8)
      mut i = 0
      while(i < n){
         store64(newp, load64(lst, 16 + i * 8), 16 + i * 8)
         i = i + 1
      }
      free(lst)
      lst = newp
   }
   store64(lst, v, 16 + n * 8)
   store64(lst, n + 1, 0)
   lst
}

fn _substr(s, start, stop){
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
      i = i + 1
   }
   store8(out, 0, len)
   out
}

fn _is_ws(c){
   if(c == 32 || c == 9 || c == 10 || c == 11 || c == 12 || c == 13){ return true }
   return false
}

fn strip(s){
   "Returns `s` without leading/trailing ASCII whitespace."
   if(!is_str(s)){ return "" }
   mut n = str_len(s)
   if(n == 0){ return "" }
   mut start = 0
   while(start < n && _is_ws(load8(s, start))){ start = start + 1 }
   mut end = n
   while(end > start && _is_ws(load8(s, end - 1))){ end = end - 1 }
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
      mut match = 1
      while(j < sep_len){
        if(load8(s, i + j) != load8(sep, j)){
          match = 0
          break
        }
         j = j + 1
      }
      if(match){
         out = _list_append(out, _substr(s, start, i))
         i = i + sep_len
         start = i
      } else {
         i = i + 1
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
      i = i + 1
   }
   mut j = 0
   while(j < n2){
      store8(out, load8(b, j), n1 + j)
      j = j + 1
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
      i = i + 1
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
      i = i + 1
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
      i = i + 1
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
   mut n = list_len(items)
   if(n == 0){ return "" }
   mut out = get(items, 0)
   mut i = 1
   while(i < n){
      out = str_add(out, sep)
      out = str_add(out, get(items, i))
      i = i + 1
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
