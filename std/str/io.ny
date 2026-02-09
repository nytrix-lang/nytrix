;; Keywords: str io
;; Basic IO helpers.

module std.str.io (
   _print_write, print
)
use std.core *
use std.core as core
use std.core.reflect *
use std.str *
use std.os.sys *

fn _write_str(s){
   "Internal: writes a raw string to stdout without conversion."
   def n = str_len(s)
   if(n > 0){ unwrap(sys_write(1, s, n)) }
}

fn _print_write(v){
   "Writes a value to stdout without a trailing newline."
   def s = is_str(v) ? v : to_str(v)
   _write_str(s)
}

fn print(...args){
   "Prints values with optional keyword args `sep` and `end`."
   mut sep = " "
   mut end = "\n"
   def n = core.len(args)
   mut vals = list(n)
   mut i = 0
   while(i < n){
      def arg = get(args, i)
      if(is_kwargs(arg)){
         def k = get_kwarg_key(arg)
         def v = get_kwarg_val(arg)
         if(eq(k, "sep")){
            sep = is_str(v) ? v : to_str(v)
         } else if(eq(k, "end")){
            end = is_str(v) ? v : to_str(v)
         }
      } else {
         vals = append(vals, arg)
      }
      i = i + 1
   }
   def m = core.len(vals)
   i = 0
   while(i < m){
      _print_write(get(vals, i))
      if(i + 1 < m){ _write_str(sep) }
      i = i + 1
   }
   _write_str(end)
   0
}
