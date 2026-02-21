;; Keywords: util inspect
;; Util Inspect module.

module std.util.inspect (
   inspect,
   repl_show
)
use std.core *
use std.core.reflect *
use std.str.io *

fn inspect(x){
   "Prints detailed information about value `x`, including its type, representation, length (if applicable), and memory address (for pointers)."
   def t = type(x)
   print(f"Type:  {t}")
   print(f"Value: {repr(x)}")
   if(t == "list" || t == "dict" || t == "set"){
      print(f"Len:   {to_str(len(x))}")
   }
   if(is_ptr(x)){
      print(f"Addr:  0x{to_str(x)}")
   }
   return 0
}

fn repl_show(x){
   "Function used by the REPL to display values."
   if(x != 0){
      print(repr(x))
   }
   x
}

if(comptime{__main()}){
    use std.util.inspect *
    use std.core.dict *
    use std.core.error *

    print("Testing inspect...")

    inspect(123)
    inspect("hello")
    inspect([1, 2, 3])
    inspect({ "a": 1 })
    inspect(0)

    print("✓ std.util.inspect tests passed")
}
