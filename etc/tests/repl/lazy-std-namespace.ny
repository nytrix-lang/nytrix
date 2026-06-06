;; repl-expect: REPL_LAZY_STD_NAMESPACE_OK
use std

fn main() int {
   assert(math.abs(-9) == 9, "lazy std root imports namespace math calls")
   assert(std.math.abs(-11) == 11, "lazy std root accepts full std math completions")
   def parts = str.split("alpha,beta", ",")
   assert(parts.len == 2, "lazy std root imports namespace str calls")
   assert(parts.get(0) == "alpha", "str namespace result is usable")
   def full_parts = std.core.str.split("red:blue", ":")
   assert(full_parts.get(1) == "blue", "lazy std root accepts full std str completions")
   assert(os.file_exists("/tmp") == true, "lazy std root imports namespace os calls")
   assert(std.os.file_exists("/tmp") == true, "lazy std root accepts full std os completions")
   assert(nt.is_prime(17) == true, "lazy std root imports nested module aliases")
   assert(std.math.nt.is_prime(19) == true, "lazy std root accepts full nested std completions")
   print("REPL_LAZY_STD_" + "NAMESPACE_OK")
   return 0
}
