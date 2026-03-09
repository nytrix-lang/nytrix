;; Keywords: inspect
;; Runtime value inspection, shape checks, and debug-friendly representation.
module std.core.inspect(inspect, repl_show)
use std.core
use std.core.reflect
use std.core.io

fn inspect(any: x): int {
   "Prints detailed information about value `x`, including its type, representation, length(if applicable), and memory address(for pointers)."
   def t = type(x)
   print(f"Type:  {t}")
   print(f"Value: {repr(x)}")
   if(t == "list" || t == "dict" || t == "set"){ print(f"Len:   {to_str(x.len)}") }
   if(is_ptr(x)){ print(f"Addr:  0x{to_str(x)}") }
   0
}

fn repl_show(any: x): any {
   "Function used by the REPL to display values."
   if(x || __is_int(x)){ print(repr(x)) }
   x
}
