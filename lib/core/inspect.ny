;; Keywords: core inspect reflection debug format
;; Runtime value inspection, shape checks, and debug-friendly representation.
;; References:
;; - std.core
module std.core.inspect(inspect, inspect_str, repl_show)
use std.core
use std.core.reflect
use std.core.io

fn inspect_str(any x) str {
   "Returns detailed information about value `x` as a string."
   def t = type(x)
   mut out = f"Type:  {t}\nValue: {repr(x)}"
   if t == "list" || t == "dict" || t == "set" { out = out + f"\nLen:   {to_str(x.len)}" }
   if is_ptr(x) { out = out + f"\nAddr:  0x{to_str(x)}" }
   out
}

fn inspect(any x) int {
   "Prints detailed information about value `x`, including its type, representation, length(if applicable), and memory address(for pointers)."
   print(inspect_str(x))
   0
}

fn repl_show(any x) any {
   "Function used by the REPL to display values."
   if x || __is_int(x) { print(repr(x)) }
   x
}

#main {
   assert(str_contains(inspect_str(123), "Type:  int"), "inspect int type")
   assert(str_contains(inspect_str("hello"), "Value: \"hello\""), "inspect str repr")
   assert(str_contains(inspect_str([1, 2, 3]), "Len:   3"), "inspect list len")
   print("✓ std.core.inspect self-test passed")
}
