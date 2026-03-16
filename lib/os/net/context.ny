;; Keywords: net context logging color
;; Shared network context and log formatting.
module std.os.net.context(
   context, set_context, set_default_level, default_level,
   level_value, level_name, timeout_ms, chunk_size,
   color_enabled, paint, log_enabled, log_line
)

use std.core
use std.core.str
use std.os.prim (env)

mut _default_level_override = ""
mut _default_timeout_ms = -1
mut _default_chunk_size = 0
mut _default_color = -1

fn _env_int_or(str: key, int: fallback): int {
   def v = env(key)
   if(is_int(v)){ return v }
   if(is_str(v)){
      def s = strip(v)
      if(s.len > 0){ return atoi(s) }
   }
   fallback
}

fn _truthy(any: v, bool: fallback=false): bool {
   if(v == nil || v == 0){ return fallback }
   if(is_int(v)){ return v != 0 }
   if(is_str(v)){
      def s = lower(strip(v))
      return !(s == "" || s == "0" || s == "false" || s == "off" || s == "no" || s == "never")
   }
   v ? true : false
}

fn _verbose_default(): bool {
   def v = env("NY_NET_VERBOSE")
   if(v == nil || v == 0){ return false }
   if(is_int(v)){ return v != 0 }
   if(!is_str(v)){ return true }
   _truthy(v, false)
}

fn level_value(any: level): int {
   "Returns numeric log severity: quiet=0, error=1, info=2, debug=3, trace=4."
   if(level == nil || level == 0){ return 0 }
   if(is_int(level)){ return level }
   if(!is_str(level)){ return level ? 2 : 0 }
   def s = lower(strip(level))
   if(s == "" || s == "quiet" || s == "none" || s == "off" || s == "false" || s == "0"){ return 0 }
   if(s == "error" || s == "err"){ return 1 }
   if(s == "info" || s == "notice"){ return 2 }
   if(s == "debug" || s == "dbg" || s == "true" || s == "1"){ return 3 }
   if(s == "trace" || s == "verbose" || s == "2"){ return 4 }
   3
}

fn level_name(any: level): str {
   "Normalizes a log level to quiet/error/info/debug/trace."
   def n = level_value(level)
   if(n <= 0){ return "quiet" }
   if(n == 1){ return "error" }
   if(n == 2){ return "info" }
   if(n == 3){ return "debug" }
   "trace"
}

fn default_level(): str {
   "Returns the process-wide network log level."
   if(is_str(_default_level_override) && _default_level_override.len > 0){ return _default_level_override }
   def lv = env("NY_NET_LOG_LEVEL")
   if(is_str(lv) && strip(lv).len > 0){ return level_name(lv) }
   _verbose_default() ? "debug" : "quiet"
}

fn set_default_level(any: level="debug"): str {
   "Sets the process-wide default network log level."
   _default_level_override = level_name(level)
   _default_level_override
}

fn timeout_ms(int: fallback=5000): int {
   "Returns the process-wide network timeout in milliseconds."
   if(_default_timeout_ms >= 0){ return _default_timeout_ms }
   _env_int_or("NY_NET_TIMEOUT_MS", fallback)
}

fn chunk_size(): int {
   "Returns the process-wide default tube chunk size, or 0 for backend default."
   _default_chunk_size
}

fn color_enabled(any: options=0): bool {
   "Returns whether network logs should use ANSI color."
   if(is_dict(options) && options.get("color", nil) != nil){ return _truthy(options.get("color"), true) }
   if(_default_color >= 0){ return _default_color != 0 }
   def no = env("NO_COLOR")
   if(no != nil && no != 0 && to_str(no).len > 0){ return false }
   def c = env("NY_NET_COLOR")
   if(is_str(c) && strip(c).len > 0){ return _truthy(c, true) }
   true
}

fn _ansi_code(str: color, int: bold=0): str {
   mut c = "0"
   if(color == "red"){ c = "31" }
   elif(color == "green"){ c = "32" }
   elif(color == "yellow"){ c = "33" }
   elif(color == "blue"){ c = "34" }
   elif(color == "magenta"){ c = "35" }
   elif(color == "cyan"){ c = "36" }
   elif(color == "gray"){ c = "90" }
   elif(color == "white"){ c = "37" }
   if(bold != 0){ return "1;" + c }
   c
}

fn paint(str: s, str: color="", int: bold=0, any: options=0): str {
   "Applies ANSI color when network color is enabled."
   if(!color_enabled(options) || color.len == 0){ return s }
   "\033[" + _ansi_code(color, bold) + "m" + s + "\033[0m"
}

fn _option_level(any: options): str {
   if(is_dict(options)){
      def lv = options.get("log_level", options.get("level", ""))
      if(is_str(lv) && strip(lv).len > 0){ return level_name(lv) }
      if(is_int(lv)){ return level_name(lv) }
   }
   default_level()
}

fn log_enabled(any: options, str: want): bool {
   "Returns true when the current/default network context enables `want`."
   level_value(_option_level(options)) >= level_value(want)
}

fn _level_color(str: lvl): str {
   if(lvl == "error"){ return "red" }
   if(lvl == "info"){ return "green" }
   if(lvl == "debug"){ return "cyan" }
   "gray"
}

fn log_line(str: tag, str: want, str: msg, any: options=0): int {
   "Prints a consistently formatted network log line when enabled."
   if(!log_enabled(options, want)){ return 0 }
   def lvl = level_name(want)
   print(paint("[" + tag + " " + lvl + "]", _level_color(lvl), lvl == "info" ? 1 : 0, options) + " " + msg)
   0
}

fn set_context(any: options=0): dict {
   "Updates process-wide network defaults."
   if(is_str(options) && strip(options).len > 0){
      set_default_level(options)
   } elif(is_dict(options)){
      if(options.get("log_level", nil) != nil){ set_default_level(options.get("log_level")) }
      if(options.get("level", nil) != nil){ set_default_level(options.get("level")) }
      if(options.get("timeout_ms", nil) != nil){ _default_timeout_ms = max(0, atoi(to_str(options.get("timeout_ms")))) }
      if(options.get("timeout", nil) != nil){ _default_timeout_ms = max(0, atoi(to_str(options.get("timeout"))) * 1000) }
      if(options.get("chunk_size", nil) != nil){ _default_chunk_size = max(0, min(1048576, atoi(to_str(options.get("chunk_size"))))) }
      if(options.get("color", nil) != nil){ _default_color = _truthy(options.get("color"), true) ? 1 : 0 }
   }
   context()
}

fn context(any: options=0): dict {
   "Returns or updates process-wide network context."
   if((is_str(options) && strip(options).len > 0) || is_dict(options)){ return set_context(options) }
   {
      "log_level": default_level(),
      "timeout_ms": _default_timeout_ms,
      "chunk_size": _default_chunk_size,
      "color": color_enabled()
   }
}

if(comptime{ __main() }){
   assert_eq(level_name("dbg"), "debug", "dbg alias")
   assert_eq(level_name("verbose"), "trace", "verbose alias")
   assert_eq(level_value("info"), 2, "info numeric level")
   def c = set_context({
         "log_level": "debug",
         "timeout_ms": 1234,
         "chunk_size": 77,
         "color": false
   })
   assert_eq(c.get("log_level", ""), "debug", "context log level")
   assert_eq(timeout_ms(0), 1234, "context timeout")
   assert_eq(chunk_size(), 77, "context chunk size")
   assert_eq(color_enabled(), false, "context color off")
   assert_eq(paint("ny", "cyan", 1), "ny", "paint disabled")
   assert_eq(log_enabled(0, "debug"), true, "debug enabled")
   print("net context self-test ok")
}
