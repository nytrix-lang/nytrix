;; Keywords: util common
;; Shared utility helpers for small cross-module behaviors.

module std.util.common (
   touch, yn, env_truthy, env_present, cached_env_truthy, cached_env_present,
   parse_nonneg_int, last_index_byte, env_hex
)

use std.core *
use std.str *
use std.os.prim *

fn touch(...args){
   "Consumes arguments intentionally and returns their count."
   len(args)
}

fn yn(v){
   "Converts a truthy value into `\"yes\"` or `\"no\"`."
   if(v){ return "yes" }
   "no"
}

fn env_truthy(name){
   "Returns whether environment variable `name` is set to a common truthy textual value."
   def v = env(name)
   if(!v){ return false }
   def s = lower(strip(v))
   s == "1" || s == "true" || s == "on" || s == "yes"
}

fn env_present(name){
   "Returns whether environment variable `name` is present and non-empty."
   def v = env(name)
   v && str_len(v) > 0
}

fn cached_env_truthy(flag, name){
   "Returns cached truthy state `flag`, initializing it from environment variable `name` when needed."
   if(flag != -1){ return flag }
   if(env_truthy(name)){ return 1 }
   0
}

fn cached_env_present(flag, name){
   "Returns cached presence state `flag`, initializing it from environment variable `name` when needed."
   if(flag != -1){ return flag }
   if(env_present(name)){ return 1 }
   0
}

fn parse_nonneg_int(v){
   "Parses a non-negative integer from string `v`, returning 0 on invalid input."
   if(!is_str(v)){ return 0 }
   def s = strip(v)
   if(str_len(s) == 0){ return 0 }
   def n = atoi(s)
   if(n < 0){ return 0 }
   n
}

fn env_hex(name, def_val){
   "Parses a hex color from environment variable `name` (e.g. '0xff123456' or '123456')."
   def v = env(name)
   if(!v || str_len(v) == 0){ return def_val }
   mut s = strip(v)
   if(startswith(s, "0x")){ s = str_slice(s, 2, str_len(s)) }
   elif(startswith(s, "#")){ s = str_slice(s, 1, str_len(s)) }

   mut res = 0
   mut i = 0 while(i < str_len(s)){
      def c = load8(s, i)
      mut val = 0
      if(c >= 48 && c <= 57){ val = c - 48 }
      elif(c >= 65 && c <= 70){ val = c - 55 }
      elif(c >= 97 && c <= 102){ val = c - 87 }
      else { break }
      res = (res << 4) | val
      i += 1
   }
   res
}

fn last_index_byte(s, want){
   "Returns the last byte index of `want` in string `s`, or `-1` when not found."
   if(!is_str(s)){ return -1 }
   mut i = str_len(s) - 1
   while(i >= 0){
      if(load8(s, i) == want){ return i }
      i -= 1
   }
   -1
}
