;; Keywords: common shared core
;; Cross-module runtime utilities used by standard-library code.
;; References:
;; - std.core
module std.core.common(touch, yn, value_or, env_trim, env_lower, env_truthy, env_falsey, env_enabled, env_toggle, env_int_clamped, env_present, cached_env_truthy, cached_env_enabled, cached_env_toggle, cached_env_present, parse_nonneg_int, last_index_byte, env_hex, parse_toggle_arg)
use std.core
use std.core.str
use std.os.prim

fn touch(...args) int {
   "Consumes arguments intentionally and returns their count."
   args.len
}

fn yn(any v) str {
   "Converts a truthy value into `\"yes\"` or `\"no\"`."
   v ? "yes" : "no"
}

fn value_or(any value, any fallback) any {
   "Returns `value` when truthy, otherwise `fallback`."
   value ? value : fallback
}

fn env_trim(str name) str {
   "Returns environment variable `name` stripped of surrounding whitespace, or empty string."
   def v = env(name)
   if(!v){ return "" }
   strip(to_str(v))
}

fn env_lower(str name) str {
   "Returns environment variable `name` stripped and lowercased, or empty string."
   lower(env_trim(name))
}

fn env_truthy(str name) bool {
   "Returns whether environment variable `name` is set to a common truthy textual value."
   case env_lower(name){
      "1", "true", "on", "yes" -> true
      _ -> false
   }
}

fn env_falsey(str name) bool {
   "Returns whether environment variable `name` is set to a common falsey textual value."
   case env_lower(name){
      "0", "false", "off", "no" -> true
      _ -> false
   }
}

fn env_enabled(str name) bool {
   "Returns true when `name` is present and not a common falsey textual value."
   case env_lower(name){
      "", "0", "false", "off", "no" -> false
      _ -> true
   }
}

fn env_toggle(str name, bool default_value=false) bool {
   "Parses an environment toggle, returning `default_value` when unset or unrecognized."
   case env_lower(name){
      "0", "false", "off", "no" -> false
      "1", "true", "on", "yes" -> true
      _ -> default_value
   }
}

fn env_int_clamped(str name, int fallback, int minimum=0, int maximum=1000000) int {
   "Parses integer env `name`, returning `fallback` when unset and clamping to [minimum, maximum]."
   def raw = env_trim(name)
   if(raw.len == 0){ return fallback }
   min(max(int(atof(raw)), minimum), maximum)
}

fn env_present(str name) bool {
   "Returns whether environment variable `name` is present and non-empty."
   env_trim(name).len > 0
}

fn cached_env_truthy(int flag, str name) int {
   "Returns cached truthy state `flag`, initializing it from environment variable `name` when needed."
   _cached_bool(flag, env_truthy(name))
}

fn cached_env_enabled(int flag, str name) int {
   "Returns cached enabled state `flag`, initializing it from environment variable `name` when needed."
   _cached_bool(flag, env_enabled(name))
}

fn cached_env_toggle(int flag, str name, bool default_value=false) int {
   "Returns cached toggle state `flag`, initializing it from environment variable `name` when needed."
   _cached_bool(flag, env_toggle(name, default_value))
}

fn cached_env_present(int flag, str name) int {
   "Returns cached presence state `flag`, initializing it from environment variable `name` when needed."
   _cached_bool(flag, env_present(name))
}

fn _cached_bool(int flag, bool value) int {
   "Returns an existing cached flag or converts `value` to the cache encoding."
   flag != -1 ? flag : (value ? 1 : 0)
}

fn parse_nonneg_int(any v) int {
   "Parses a non-negative integer from string `v`, returning 0 on invalid input."
   if(!is_str(v)){ return 0 }
   def s = strip(v)
   if(s.len == 0){ return 0 }
   def n = atoi(s)
   if(n < 0){ return 0 }
   n
}

fn env_hex(str name, int def_val) int {
   "Parses a hex color from environment variable `name` (e.g. '0xff123456' or '123456')."
   def v = env(name)
   if(!v || v.len == 0){ return def_val }
   mut s = strip(v)
   if(startswith(s, "0x")){ s = str_slice(s, 2, s.len) }
   elif(startswith(s, "#")){ s = str_slice(s, 1, s.len) }
   mut res = 0
   mut i = 0 while(i < s.len){
      def c = load8(s, i)
      def val = case c {
         48..57 -> c - 48
         65..70 -> c - 55
         97..102 -> c - 87
         _ -> -1
      }
      if(val < 0){ break }
      res = (res << 4) | val
      i += 1
   }
   res
}

fn last_index_byte(str s, int want) int {
   "Returns the last byte index of `want` in string `s`, or `-1` when not found."
   if(!is_str(s)){ return -1 }
   mut i = s.len - 1
   while(i >= 0){
      if(load8(s, i) == want){ return i }
      i -= 1
   }
   -1
}

fn parse_toggle_arg(list parts, bool cur, bool default_next) bool {
   "Parses common on/off/toggle arguments from `parts`, otherwise returns `default_next`."
   if(parts.len <= 1){ return default_next }
   def arg = lower(parts.get(1, ""))
   case arg {
      "on", "1", "true", "yes" -> true
      "off", "0", "false", "no" -> false
      "toggle", "tog" -> !cur
      _ -> default_next
   }
}
