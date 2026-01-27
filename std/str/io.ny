;; Keywords: str io
;; Basic IO helpers.

use std.core *
use std.core.reflect *
use std.str *
use std.os.sys *

module std.str.io (
   _print_write, print
)

fn _write_str(s){
   def n = str_len(s)
   if(n > 0){ sys_write(1, s, n) }
}

fn _print_write(v){
   "Writes a value to stdout without a trailing newline."
   def s = is_str(v) ? v : to_str(v)
   _write_str(s)
}

fn print(...args){
   "Prints all arguments separated by spaces and ends with a newline."
   def n = list_len(args)
   mut i = 0
   while(i < n){
      _print_write(get(args, i))
      if(i + 1 < n){ sys_write(1, " ", 1) }
      i = i + 1
   }
   sys_write(1, "\n", 1)
   0
}

