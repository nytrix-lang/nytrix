;; Keywords: re regex
;; Regular-expression matching and text search operations.
module std.core.re(search, match_start, matches, contains, findall, sub, split_alt)
use std.core
use std.core.str as str

fn split_alt(str: pattern): list {
   "Splits a lightweight pattern on `|`. This is literal alternation, not full PCRE."
   if(pattern.len == 0){ return [""] }
   str.split(pattern, "|")
}

fn search(str: pattern, any: text): bool {
   "Returns true when `pattern` appears in `text`.
   Supports literal substring search plus `a|b|c` alternation for scripting."
   if(!is_str(text)){ text = to_str(text) }
   def parts = split_alt(pattern)
   mut i = 0
   while(i < parts.len){
      def p = parts.get(i, "")
      if(p.len == 0 || str.find(text, p) >= 0){ return true }
      i += 1
   }
   false
}

fn contains(any: text, str: pattern): bool {
   "Alias-friendly spelling for `search(pattern, text)`."
   search(pattern, text)
}

fn match_start(str: pattern, any: text): bool {
   "Returns true when `text` starts with any literal alternative in `pattern`."
   if(!is_str(text)){ text = to_str(text) }
   def parts = split_alt(pattern)
   mut i = 0
   while(i < parts.len){
      def p = parts.get(i, "")
      if(p.len == 0 || str.startswith(text, p)){ return true }
      i += 1
   }
   false
}

fn matches(str: pattern, any: text): bool {
   "Returns true when `pattern` matches at the start of `text`."
   match_start(pattern, text)
}

fn findall(str: pattern, any: text): list {
   "Returns literal alternatives from `pattern` that occur in `text`, in pattern order."
   if(!is_str(text)){ text = to_str(text) }
   mut out = []
   def parts = split_alt(pattern)
   mut i = 0
   while(i < parts.len){
      def p = parts.get(i, "")
      if(p.len == 0 || str.find(text, p) >= 0){ out = out.append(p) }
      i += 1
   }
   out
}

fn sub(str: pattern, any: repl, any: text): str {
   "Replaces each literal alternative in `pattern` with `repl`."
   if(!is_str(text)){ text = to_str(text) }
   if(!is_str(repl)){ repl = to_str(repl) }
   mut out = text
   def parts = split_alt(pattern)
   mut i = 0
   while(i < parts.len){
      def p = parts.get(i, "")
      if(p.len > 0){ out = str.str_replace(out, p, repl) }
      i += 1
   }
   out
}
