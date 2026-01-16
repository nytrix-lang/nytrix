;; Keywords: util json
;; Util Json module.

use std.core
use std.core.reflect
use std.strings.str
use std.collections
use std.collections.dict
use std.util.convert
module std.util.json (
   _json_hex2, json_escape, json_encode, json_of, _json_skip_ws, _json_parse_val,
   _json_parse_str, _json_parse_num, _json_parse_list, _json_init_dict, json_decode
)

fn _json_hex2(n){
   "Internal: two-digit lowercase hex for a byte."
   def h = "0123456789abcdef"
   return f"{slice(h, n >> 4, (n >> 4) + 1)}{slice(h, n & 15, (n & 15) + 1)}"
}

fn json_escape(s){
   "Escapes control characters and double quotes in string `s` for JSON compatibility."
   def out = list(8)
   def i = 0  def n = str_len(s)
   while(i < n){
      def c = load8(s, i)
      out = append(out, case c {
         34 -> "\\\""
         92 -> "\\\\"
         10 -> "\\n"
         9  -> "\\t"
         13 -> "\\r"
         _  -> {
            if(c < 32){ f"\\u00{_json_hex2(c)}" }
            else { chr(c) }
         }
      })
      i += 1
   }
   join(out, "")
}

fn json_encode(v){
   "Serializes Nytrix value `v` into its JSON string representation."
   case type(v) {
     "list", "tuple" -> {
      def out = "["
      def i = 0  def n = list_len(v)
      while(i < n){
         out = f"{out}{json_encode(get(v, i))}"
         if(i + 1 < n){ out = f"{out}," }
         i += 1
      }
      f"{out}]"
     }
     "dict" -> {
      def out = "{"
      def its = items(v)
      def i = 0  def n = list_len(its)
      while(i < n){
         def p = get(its, i)
         def k = get(p, 0)
         def val = get(p, 1)
         out = f"{out}\"{json_escape(k)}\":{json_encode(val)}"
         if(i + 1 < n){ out = f"{out}," }
         i = i + 1
      }
      f"{out}}"
     }
     "str" -> f"\"{json_escape(v)}\""
     "int" -> to_str(v)
     "float" -> __to_str(v)
     "bool" -> case v { true -> "true" _ -> "false" }
     "none" -> "null"
     _      -> repr(v)
   }
}

fn json_of(self){
   "Method form of json_encode."
   return json_encode(self)
}

fn _json_skip_ws(s, i, n){
   "Internal: skip JSON whitespace and return next index."
   while(i < n){
      def c = load8(s, i)
      if(c != 32 && c != 10 && c != 13 && c != 9){ return i }
      i = i + 1
   }
   return i
}

fn _json_parse_val(s, i, n){
   "Internal: parse JSON value, return [value, next_idx]."
   i = _json_skip_ws(s, i, n)
   if(i >= n){ return [0, i] }
   def c = load8(s, i)
   return case c {
      34 -> _json_parse_str(s, i, n)
      91 -> _json_parse_list(s, i, n)
      123 -> _json_init_dict(s, i, n)
      116 -> [true, i + 4] ; "true"
      102 -> [false, i + 5] ; "false"
      110 -> [0, i + 4] ; "null"
      _ -> _json_parse_num(s, i, n)
   }
}

fn _json_parse_str(s, i, n){
   "Internal: parse JSON string, return [str, next_idx]."
   i = i + 1 "skip opening quote"
   def res = ""
   while(i < n){
      def c = load8(s, i)
      if(c == 34){ return [res, i + 1] }
      if(c == 92){ "escape"
         i = i + 1
         def c2 = load8(s, i)
         def esc = case c2 {
            110 -> "\n"
            116 -> "\t"
            114 -> "\r"
            34 -> "\""
            92 -> "\\"
            _ -> chr(c2)
         }
         res = f"{res}{esc}"
      } else {
         res = f"{res}{chr(c)}"
      }
      i = i + 1
   }
   panic("json: unterminated string")
}

fn _json_parse_num(s, i, n){
   "Internal: parse JSON integer, return [int, next_idx]."
   def start = i
   while(i < n){
      def c = load8(s, i)
      if(c < 48 || c > 57){
         if(i == start && c == 45){ i = i + 1  continue }
         break
      }
      i = i + 1
   }
   def num_str = ""
   def j = start
   while(j < i){
      num_str = f"{num_str}{chr(load8(s, j))}"
      j = j + 1
   }
   def val = parse_int(num_str)
   return [val, i]
}

fn _json_parse_list(s, i, n){
   "Internal: parse JSON array, return [list, next_idx]."
   i = i + 1 "skip ["
   def res = list(8)
   while(i < n){
      i = _json_skip_ws(s, i, n)
      if(load8(s, i) == 93){ return [res, i + 1] }
      def p = _json_parse_val(s, i, n)
      res = append(res, get(p, 0))
      i = get(p, 1)
      i = _json_skip_ws(s, i, n)
      if(load8(s, i) == 44){ i = i + 1 } "skip ,"
   }
   panic("json: unterminated list")
}

fn _json_init_dict(s, i, n){
   "Internal: parse JSON object, return [dict, next_idx]."
   i = i + 1 "skip brace"
   def res = dict(16)
   while(i < n){
      i = _json_skip_ws(s, i, n)
      if(load8(s, i) == 125){ return [res, i + 1] }
      def p_key = _json_parse_str(s, i, n)
      def key = get(p_key, 0)
      i = get(p_key, 1)
      i = _json_skip_ws(s, i, n)
      if(load8(s, i) != 58){ panic("json: expected : in dict") }
      i = i + 1
      def p_val = _json_parse_val(s, i, n)
      res = dict_set(res, key, get(p_val, 0))
      i = get(p_val, 1)
      i = _json_skip_ws(s, i, n)
      if(load8(s, i) == 44){ i = i + 1 } "skip ,"
   }
   panic("json: unterminated dict")
}

fn json_decode(s){
   "Parse JSON string to value."
   def p = _json_parse_val(s, 0, str_len(s))
   return get(p, 0)
}