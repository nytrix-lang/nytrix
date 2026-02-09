;; Keywords: core debug
;; Core Debug module.

module std.core.debug (
   debug_print_val, debug_print
)
use std.core *
use std.core.reflect *
use std.str.io *
use std.str *

fn debug_print_val(val){
   "Prints a detailed debug representation of a single value."
   _print_write("Value(raw: ")
   _print_write(to_str(val))
   _print_write(", type: ")
   _print_write(type(val))
   if(is_ptr(val)){
      _print_write(", addr: ")
      _print_write(to_str(val))
   }
   _print_write(")\n")
}

fn debug_print(...args){
   "Prints a detailed debug representation of one or more values."
   mut xs = args
   if(len(args) == 1){
      def first = get(args, 0)
      if(eq(type(first), "list")){ xs = first }
   }
   def n = len(xs)
   mut i = 0
   while(i < n){
      def v = get(xs, i)
      debug_print_val(v)
      i = i + 1
   }
}

