;; Keywords: util inspect
;; Util Inspect module.

module std.util.inspect (
   inspect,
   repl_show
)
use std

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
