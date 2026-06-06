;; Keywords: core debug diagnostics trace inspect
;; Core Debug for Nytrix
;; References:
;; - std.core
module std.core.debug(debug_print_val, debug_print, breakpoint)
use std.core
use std.core.io

fn debug_print_val(any val) any {
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

fn debug_print(...args) any {
   "Prints a detailed debug representation of one or more values."
   mut xs = args
   if(args.len == 1){
      def first = args.get(0)
      if(is_list(first)){ xs = first }
   }
   def n = xs.len
   mut i = 0
   while(i < n){
      def v = xs.get(i)
      debug_print_val(v)
      i += 1
   }
}

fn breakpoint() any {
   "Triggers a debugger trap on supported architectures."
   __breakpoint()
}
